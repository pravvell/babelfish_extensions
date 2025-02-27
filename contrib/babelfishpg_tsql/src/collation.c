#include "postgres.h"

#include "collation.h"
#include "fmgr.h"
#include "guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "catalog/pg_collation.h"
#include "catalog/namespace.h"
#include "tsearch/ts_locale.h"
#include "parser/parser.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"
#include "parser/parse_oper.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include <unicode/utrans.h>

#include "pltsql.h"
#include "src/collation.h"

#define NOT_FOUND -1
#define SORT_KEY_STR "\357\277\277\0"

/* 
 * Rule applied to transliterate Latin and general category Nd character 
 * then convert the Latin (source) char to ASCII (destination) representation
 */
#define TRANSFORMATION_RULE "[[:Latin:][:Nd:]]; Latin-ASCII"

/*
 * The maximum number of bytes per character is 4 according 
 * to RFC3629 which limited the character table to U+10FFFF
 * Ref: https://www.rfc-editor.org/rfc/rfc3629#section-3
 */
#define MAX_BYTES_PER_CHAR 4
#define MAX_INPUT_LENGTH_TO_REMOVE_ACCENTS 250 * 1024 * 1024

Oid			server_collation_oid = InvalidOid;
collation_callbacks *collation_callbacks_ptr = NULL;
extern bool babelfish_dump_restore;
static Oid remove_accents_internal_oid;
static UTransliterator *cached_transliterator = NULL;

static Node *pgtsql_expression_tree_mutator(Node *node, void *context);
static void init_and_check_collation_callbacks(void);

extern int	pattern_fixed_prefix_wrapper(Const *patt,
										 int ptype,
										 Oid collation,
										 Const **prefix,
										 Selectivity *rest_selec);

/* pattern prefix status for pattern_fixed_prefix_wrapper
 * Pattern_Prefix_None: no prefix found, this means the first character is a wildcard character
 * Pattern_Prefix_Exact: the pattern doesn't include any wildcard character
 * Pattern_Prefix_Partial: the pattern has a constant prefix
 */
typedef enum
{
	Pattern_Prefix_None, Pattern_Prefix_Partial, Pattern_Prefix_Exact
} Pattern_Prefix_Status;

PG_FUNCTION_INFO_V1(init_collid_trans_tab);
PG_FUNCTION_INFO_V1(init_like_ilike_table);
PG_FUNCTION_INFO_V1(get_server_collation_oid);
PG_FUNCTION_INFO_V1(is_collated_ci_as_internal);

/* this function is no longer needed and is only a placeholder for upgrade script */
PG_FUNCTION_INFO_V1(init_server_collation);
Datum
init_server_collation(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(0);
}

/* this function is no longer needed and is only a placeholder for upgrade script */
PG_FUNCTION_INFO_V1(init_server_collation_oid);
Datum
init_server_collation_oid(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(0);
}

/* init_collid_trans_tab - this function is no longer needed and is only a placeholder for upgrade script */
Datum
init_collid_trans_tab(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(0);
}

PG_FUNCTION_INFO_V1(collation_list);

Datum
collation_list(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(tsql_collation_list_internal(fcinfo));
}


/*
 * get_server_collation_oid - this is being used by sys.babelfish_update_collation_to_default
 * to update the collation of system objects
 */
Datum
get_server_collation_oid(PG_FUNCTION_ARGS)
{
	PG_RETURN_OID(tsql_get_server_collation_oid_internal(false));
}


Datum
is_collated_ci_as_internal(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(tsql_is_collated_ci_as_internal(fcinfo));
}

/* init_like_ilike_table - this function is no longer needed and is only a placeholder for upgrade script */
Datum
init_like_ilike_table(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(0);
}

static Expr *
make_op_with_func(Oid opno, Oid opresulttype, bool opretset,
				  Expr *leftop, Expr *rightop,
				  Oid opcollid, Oid inputcollid, Oid oprfuncid)
{
	OpExpr	   *expr = (OpExpr *) make_opclause(opno,
												opresulttype,
												opretset,
												leftop,
												rightop,
												opcollid,
												inputcollid);

	expr->opfuncid = oprfuncid;
	return (Expr *) expr;
}

/* helper fo make or qual, simialr to make_and_qual  */
static Node *
make_or_qual(Node *qual1, Node *qual2)
{
	if (qual1 == NULL)
		return qual2;
	if (qual2 == NULL)
		return qual1;
	return (Node *) make_orclause(list_make2(qual1, qual2));
}

static Node *
transform_funcexpr(Node *node)
{
	if (node && IsA(node, FuncExpr))
	{
		FuncExpr   *fe = (FuncExpr *) node;
		int			collidx_of_cs_as;

		if (fe->funcid == 868 || //strpos - see pg_proc.dat
		/* fe->funcid == 394  ||  // string_to_array, 3-arg form */
		/* fe->funcid == 376  ||  // string_to_array, 2-arg form */
			fe->funcid == 2073 || //substring - 2 - arg form, see pg_proc.dat
			fe->funcid == 2074 || //substring - 3 - arg form, see pg_proc.dat

			fe->funcid == 2285 || //regexp_replace, flags in 4 th arg
			fe->funcid == 3397 || //regexp_match(find first match), flags in 3 rd arg
			fe->funcid == 2764)
			/* regexp_matches, flags in 3 rd arg */
		{
			coll_info_t coll_info_of_inputcollid = tsql_lookup_collation_table_internal(fe->inputcollid);
			Node	   *leftop = (Node *) linitial(fe->args);
			Node	   *rightop = (Node *) lsecond(fe->args);

			if (OidIsValid(coll_info_of_inputcollid.oid) &&
				coll_info_of_inputcollid.collateflags == 0x000d /* CI_AS  */ )
			{
				Oid			lower_funcid = 870;

				/* lower */
				Oid result_type = 25;

				/* text */

				tsql_get_server_collation_oid_internal(true);

				if (!OidIsValid(server_collation_oid))
					return node;

				/*
				 * Find the CS_AS collation corresponding to the CI_AS
				 * collation Change the collation of the func op to the CS_AS
				 * collation
				 */
				collidx_of_cs_as =
					tsql_find_cs_as_collation_internal(
													   tsql_find_collation_internal(coll_info_of_inputcollid.collname));

				if (NOT_FOUND == collidx_of_cs_as)
					return node;

				if (fe->funcid == 2285 || fe->funcid == 3397 || fe->funcid == 2764)
				{
					Node	   *flags = (fe->funcid == 2285) ? lfourth(fe->args) : lthird(fe->args);

					if (!IsA(flags, Const))
						return node;
					else
					{
						char	   *patt = TextDatumGetCString(((Const *) flags)->constvalue);
						int			f = 0;

						while (patt[f] != '\0')
						{
							if (patt[f] == 'i')
								break;

							f++;
						}

						/*
						 * If the 'i' flag was specified then the operation is
						 * case-insensitive and so the ci_as collation may be
						 * replaced with the corresponding deterministic cs_as
						 * collation. If not, return.
						 */
						if (patt[f] != 'i')
							return node;
					}
				}

				fe->inputcollid = tsql_get_oid_from_collidx(collidx_of_cs_as);

				if (fe->funcid >= 2285)
					return node;

				/*
				 * regexp operators have their own way to handle case
				 * -insensitivity
				 */

				if (!IsA(leftop, FuncExpr) || ((FuncExpr *) leftop)->funcid != lower_funcid)
					leftop = (Node *) makeFuncExpr(lower_funcid,
												   result_type,
												   list_make1(leftop),
												   fe->inputcollid,
												   fe->inputcollid,
												   COERCE_EXPLICIT_CALL);
				if (!IsA(rightop, FuncExpr) || ((FuncExpr *) rightop)->funcid != lower_funcid)
					rightop = (Node *) makeFuncExpr(lower_funcid,
													result_type,
													list_make1(rightop),
													fe->inputcollid,
													fe->inputcollid,
													COERCE_EXPLICIT_CALL);

				if (list_length(fe->args) == 3)
				{
					Node	   *thirdop = (Node *) makeFuncExpr(lower_funcid,
																result_type,
																list_make1(lthird(fe->args)),
																fe->inputcollid,
																fe->inputcollid,
																COERCE_EXPLICIT_CALL);

					fe->args = list_make3(leftop, rightop, thirdop);
				}
				else if (list_length(fe->args) == 2)
				{
					fe->args = list_make2(leftop, rightop);
				}
			}
		}
	}

	return node;
}

/*
 * If the node is OpExpr and the colaltion is ci_as, then
 * transform the LIKE OpExpr to ILIKE OpExpr:
 *
 * Case 1: if the pattern is a constant stirng
 *		 col LIKE PATTERN -> col = PATTERN
 * Case 2: if the pattern have a constant prefix
 *		 col LIKE PATTERN ->
 *		 col LIKE PATTERN BETWEEN prefix AND prefix||E'\uFFFF'
 * Case 3: if the pattern doesn't have a constant prefix
 *		 col LIKE PATTERN -> col ILIKE PATTERN
 */

static Node *
transform_from_ci_as_for_likenode(Node *node, OpExpr *op, like_ilike_info_t like_entry, coll_info_t coll_info_of_inputcollid)
{
	Node	   *leftop = (Node *) linitial(op->args);
	Node	   *rightop = (Node *) lsecond(op->args);
	Oid			ltypeId = exprType(leftop);
	Oid			rtypeId = exprType(rightop);
	char	   *op_str;
	Node	   *ret;
	Const	   *patt;
	Const	   *prefix;
	Operator	optup;
	Pattern_Prefix_Status pstatus;
	int			collidx_of_cs_as;

	tsql_get_server_collation_oid_internal(true);

	if (!OidIsValid(server_collation_oid))
		return node;


	/*
	 * Find the CS_AS collation corresponding to the CI_AS collation
	 * Change the collation of the ILIKE op to the CS_AS collation
	 */
	collidx_of_cs_as =
		tsql_find_cs_as_collation_internal(
											tsql_find_collation_internal(coll_info_of_inputcollid.collname));


	/*
	 * A CS_AS collation should always exist unless a Babelfish CS_AS
	 * collation was dropped or the lookup tables were not defined in
	 * lexicographic order.  Program defensively here and just do no
	 * transformation in this case, which will generate a
	 * 'nondeterministic collation not supported' error.
	 */

	if (NOT_FOUND == collidx_of_cs_as)
	{
		elog(DEBUG2, "No corresponding CS_AS collation found for collation \"%s\"", coll_info_of_inputcollid.collname);
		return node;
	}

	/* Change the opno and oprfuncid to ILIKE */
	op->opno = like_entry.ilike_oid;
	op->opfuncid = like_entry.ilike_opfuncid;

	op->inputcollid = tsql_get_oid_from_collidx(collidx_of_cs_as);

	/* 
	 * This is needed to process CI_AI for Const nodes
	 * Because after we call coerce_to_target_type for type conversion in transform_likenode_for_AI,
	 * we obtain a Relabel node which won't help us to perform optimization
	 * for constant prefix. Hence, we process that here
	 */
	if (IsA(rightop, RelabelType))
	{
		RelabelType		*relabel = (RelabelType *) rightop;
		if (IsA(relabel->arg, Const))
		{
			lsecond(op->args) = relabel->arg;
			rightop = (Node *) lsecond(op->args);
		}
	}

	/* no constant prefix found in pattern, or pattern is not constant */
	if (IsA(leftop, Const) || !IsA(rightop, Const) ||
		((Const *) rightop)->constisnull)
	{
		return node;
	}

	patt = (Const *) rightop;

	/* extract pattern */
	pstatus = pattern_fixed_prefix_wrapper(patt, 1, coll_info_of_inputcollid.oid,
											&prefix, NULL);

	/* If there is no constant prefix then there's nothing more to do */
	if (pstatus == Pattern_Prefix_None)
	{
		return node;
	}

	/*
	 * If we found an exact-match pattern, generate an "=" indexqual.
	 */
	if (pstatus == Pattern_Prefix_Exact)
	{
		op_str = like_entry.is_not_match ? "<>" : "=";
		optup = compatible_oper(NULL, list_make1(makeString(op_str)), ltypeId, ltypeId,
								true, -1);
		if (optup == (Operator) NULL)
			return node;

		ret = (Node *) (make_op_with_func(oprid(optup), BOOLOID, false,
											(Expr *) leftop, (Expr *) prefix,
											InvalidOid, coll_info_of_inputcollid.oid, oprfuncid(optup)));

		ReleaseSysCache(optup);
	}
	else
	{
		Expr	   *greater_equal,
					*less_equal,
					*concat_expr;
		Node	   *constant_suffix;
		Const	   *highest_sort_key;

		/* construct leftop >= pattern */
		optup = compatible_oper(NULL, list_make1(makeString(">=")), ltypeId, ltypeId,
								true, -1);
		if (optup == (Operator) NULL)
			return node;
		greater_equal = make_op_with_func(oprid(optup), BOOLOID, false,
											(Expr *) leftop, (Expr *) prefix,
											InvalidOid, coll_info_of_inputcollid.oid, oprfuncid(optup));
		ReleaseSysCache(optup);
		/* construct pattern||E'\uFFFF' */
		highest_sort_key = makeConst(TEXTOID, -1, coll_info_of_inputcollid.oid, -1,
										PointerGetDatum(cstring_to_text(SORT_KEY_STR)), false, false);

		optup = compatible_oper(NULL, list_make1(makeString("||")), rtypeId, rtypeId,
								true, -1);
		if (optup == (Operator) NULL)
			return node;
		concat_expr = make_op_with_func(oprid(optup), rtypeId, false,
										(Expr *) prefix, (Expr *) highest_sort_key,
										InvalidOid, coll_info_of_inputcollid.oid, oprfuncid(optup));
		ReleaseSysCache(optup);
		/* construct leftop < pattern */
		optup = compatible_oper(NULL, list_make1(makeString("<")), ltypeId, ltypeId,
								true, -1);
		if (optup == (Operator) NULL)
			return node;

		less_equal = make_op_with_func(oprid(optup), BOOLOID, false,
										(Expr *) leftop, (Expr *) concat_expr,
										InvalidOid, coll_info_of_inputcollid.oid, oprfuncid(optup));
		constant_suffix = make_and_qual((Node *) greater_equal, (Node *) less_equal);
		if (like_entry.is_not_match)
		{
			constant_suffix = (Node *) make_notclause((Expr *) constant_suffix);
			ret = make_or_qual(node, constant_suffix);
		}
		else
		{
			constant_suffix = make_and_qual((Node *) greater_equal, (Node *) less_equal);
			ret = make_and_qual(node, constant_suffix);
		}
		ReleaseSysCache(optup);
	}
	return ret;
}

static void
get_remove_accents_internal_oid()
{
	const Oid funcargtypes[1] = {TEXTOID};
	if (OidIsValid(remove_accents_internal_oid))
		return;

	remove_accents_internal_oid = LookupFuncName(list_make2(makeString("sys"), makeString("remove_accents_internal")), -1, funcargtypes, true);
}

/*
 * Function responsible for obtaining unaccented version of input
 * string with the help of ICU provided APIs. 
 * We use a transformation rule to transliterate the string
 */

PG_FUNCTION_INFO_V1(remove_accents_internal);
Datum remove_accents_internal(PG_FUNCTION_ARGS)
{
	char *input_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
	UChar *utf16_input, *utf16_res;
	int32_t len_uinput, limit, capacity, len_result;
	char *result;
	UErrorCode status = U_ZERO_ERROR;
	text *res_str;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

#ifdef USE_ICU
	// Check if transliterator is not yet cached
	if (!cached_transliterator)
	{
		MemoryContext oldcontext;
		UChar *rules;
		int32_t len_uchar;

		// Switch to TopMemoryContext for allocating cached transliterator
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);

		// Load transliterator rules
		len_uchar = icu_to_uchar(&rules, TRANSFORMATION_RULE, strlen(TRANSFORMATION_RULE));

		// Open transliterator
		cached_transliterator = utrans_openU(rules, len_uchar, UTRANS_FORWARD, NULL, 0, NULL, &status);
		if (U_FAILURE(status) || !cached_transliterator)
		{
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
						errmsg("Error opening transliterator: %s", u_errorName(status))));
		}

		// Switch back to original memory context
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * XXX: Currently, we are allowing length of input string upto 250MB bytes. For long term,
	 * we should try to chunk the input string into smaller parts, remove the accents of that
	 * part and concat back the final string.
	 */
	if (strlen(input_str) > MAX_INPUT_LENGTH_TO_REMOVE_ACCENTS)
	{
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					errmsg("Input string of the length greater than 250MB is not supported by the function remove_accents_internal." \
							" This function might be used internally by LIKE operator.")));
	}

	len_uinput = icu_to_uchar(&utf16_input, input_str, strlen(input_str));

	limit = len_uinput;
	/* 
	 * set the capacity (In UChar terms) to limit * MAX_BYTES_PER_CHAR if it is less than INT32_MAX
	 * else set it to INT32_MAX as capacity is of int32_t datatype so it can have maximum INT32_MAX
	 * value which would be equivalent to 2GB UChar points and 2GB * sizeof(UChar) in byte terms.
	 * XXX: It is assumed that this capacity should handle almost all the general input strings.
	 */
	capacity = (limit < (PG_INT32_MAX / MAX_BYTES_PER_CHAR)) ? (limit * MAX_BYTES_PER_CHAR) : PG_INT32_MAX;

	/*
	 * utrans_transUChars will modify input string in place so ensure that it has enough capacity to store
	 * transformed string.
	 */
	utf16_res = (UChar *) palloc0(capacity * sizeof(UChar));
	/*
	 * utf16_input would have one NULL terminator at the end. Copy that too. Limiting memory copy to min of
	 * (len_uinput + 1) * sizeof(UChar) and capacity * sizeof(UChar) in order to avoid buffer overwriting.
	 */
	memcpy(utf16_res, utf16_input, Min((len_uinput + 1) * sizeof(UChar), capacity * sizeof(UChar)));
	pfree(utf16_input);
	pfree(input_str);

	utrans_transUChars(cached_transliterator,
						utf16_res,
						&len_uinput,
						capacity,
						0,
						&limit,
						&status);

	/* Allocated capacity may not be enough to hold un-accented string. This shouldn't occur ideally but still defensive code. */
	if (status == U_BUFFER_OVERFLOW_ERROR)
	{
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					errmsg("Buffer overflow occurred while normalising the string. Error: %s", u_errorName(status))));
	}

	if (U_FAILURE(status))
	{
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					errmsg("Error normalising the input string: %s", u_errorName(status))));
	}

	len_result = icu_from_uchar(&result, utf16_res, len_uinput);
	pfree(utf16_res);

	// Return result as NVARCHAR
	res_str = cstring_to_text_with_len(result, len_result);
	pfree(result);
	PG_RETURN_VARCHAR_P(res_str);
#else
	ereport(ERROR,
			(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				errmsg("ICU library is required to be installed in order to use the function remove_accents_internal")));
	PG_RETURN_NULL();
#endif
}

static Node *
convert_node_to_funcexpr_for_like(Node *node)
{
	FuncExpr *newFuncExpr = makeNode(FuncExpr);
	Node *new_node;
	newFuncExpr->funcid = remove_accents_internal_oid;
	newFuncExpr->funcresulttype = get_sys_varcharoid();

	if (node == NULL)
		return node;

	switch (nodeTag(node))
	{
		case T_Const:
			{
				Const *con;
				new_node = coerce_to_target_type(NULL, (Node *) node, exprType(node),
													TEXTOID, -1,
													COERCION_EXPLICIT,
													COERCE_EXPLICIT_CAST,
													exprLocation(node));
				if (unlikely(new_node == NULL))
				{
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
								errmsg("Could not type cast the input argument of LIKE operator to desired data type")));
				}

				if (IsA(new_node, Const))
				{
					con = (Const *) new_node;
					if (con->constisnull)
						return new_node;
					con->constvalue = DirectFunctionCall1(remove_accents_internal, con->constvalue);
					return (Node *) con;
				}
				else
				{
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Could not convert Const node to desired node type")));
				}
				return new_node;
			}
		case T_FuncExpr:
		case T_Var:
		case T_Param:
		case T_CaseExpr:
		case T_RelabelType:
		case T_CoerceViaIO:
		case T_CollateExpr:
			{
				new_node = coerce_to_target_type(NULL, (Node *) node, exprType(node),
													TEXTOID, -1,
													COERCION_EXPLICIT,
													COERCE_EXPLICIT_CAST,
													exprLocation(node));
				if (unlikely(new_node == NULL))
				{
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
								errmsg("Could not type cast the input argument of LIKE operator to desired data type")));
				}
				newFuncExpr->args = list_make1(new_node);
				break;
			}
		case T_SubLink:
			{
				new_node = coerce_to_target_type(NULL, (Node *) node, exprType(node),
													TEXTOID, -1,
													COERCION_EXPLICIT,
													COERCE_EXPLICIT_CAST,
													exprLocation(node));
				if (unlikely(new_node == NULL))
				{
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
								errmsg("Could not type cast the input argument of LIKE operator to desired data type")));
				}
				new_node = expression_tree_mutator(new_node, pgtsql_expression_tree_mutator, NULL);
				newFuncExpr->args = list_make1(new_node);
				break;
			}

		default:
			{
				ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("unrecognized node type: %d", (int) nodeTag(node))));
			}
	}
	return (Node *) newFuncExpr;
}


static Node *
transform_likenode_for_AI(Node *node, OpExpr *op)
{
	Node		*leftop = (Node *) linitial(op->args);
	Node		*rightop = (Node *) lsecond(op->args);

	linitial(op->args) = coerce_to_target_type(NULL,
												convert_node_to_funcexpr_for_like(leftop),
												get_sys_varcharoid(),
												exprType(leftop), -1,
												COERCION_EXPLICIT,
												COERCE_EXPLICIT_CAST,
												-1);
	lsecond(op->args) = coerce_to_target_type(NULL,
												convert_node_to_funcexpr_for_like(rightop),
												get_sys_varcharoid(),
												exprType(rightop), -1,
												COERCION_EXPLICIT,
												COERCE_EXPLICIT_CAST,
												-1);
	return node;
}

/*
 * To handle CS_AI collation for LIKE, we simply find the corresponding CS_AS collation
 * and modify the nodes by removing accents from them
 */

static Node *
transform_from_cs_ai_for_likenode(Node *node, OpExpr *op, like_ilike_info_t like_entry, coll_info_t coll_info_of_inputcollid)
{
	int			collidx_of_cs_as;

	tsql_get_server_collation_oid_internal(true);

	if (!OidIsValid(server_collation_oid))
		return node;

	/*
	 * Find the CS_AS collation corresponding to the CS_AI collation
	 */
	collidx_of_cs_as =
		tsql_find_cs_as_collation_internal(
											tsql_find_collation_internal(coll_info_of_inputcollid.collname));


	/*
	 * A CS_AS collation should always exist unless a Babelfish CS_AS
	 * collation was dropped or the lookup tables were not defined in
	 * lexicographic order.  Program defensively here and just do no
	 * transformation in this case, which will generate a
	 * 'nondeterministic collation not supported' error.
	 */
	if (NOT_FOUND == collidx_of_cs_as)
	{
		elog(DEBUG2, "No corresponding CS_AS collation found for collation \"%s\"", coll_info_of_inputcollid.collname);
		return node;
	}

	op->inputcollid = tsql_get_oid_from_collidx(collidx_of_cs_as);

	return transform_likenode_for_AI(node, op);	
}

static bool
supported_AI_collation_for_like(int32_t code_page)
{
	if (code_page == 1250 || code_page == 1252 || code_page == 1257)
		return true;
	return false;
}

static Node *
transform_likenode(Node *node)
{
	if (node && IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		like_ilike_info_t like_entry = tsql_lookup_like_ilike_table_internal(op->opno);
		coll_info_t coll_info_of_inputcollid = tsql_lookup_collation_table_internal(op->inputcollid);

		get_remove_accents_internal_oid();

		/*
		 * We do not allow CREATE TABLE statements with CHECK constraint where
		 * the constraint has an ILIKE operator and the collation is ci_as.
		 * But during dump and restore, this kind of a table definition may be
		 * generated. At this point we know that any tables being restored
		 * that match this pattern are generated by pg_dump, and not created
		 * by a user. So, it is safe to go ahead with replacing the ci_as
		 * collation with a corresponding cs_as one if an ILIKE node is found
		 * during dump and restore.
		 */
		init_and_check_collation_callbacks();
		if ((*collation_callbacks_ptr->has_ilike_node) (node) && babelfish_dump_restore)
		{
			int			collidx_of_cs_as;

			if (coll_info_of_inputcollid.oid != InvalidOid)
			{
				collidx_of_cs_as =
					tsql_find_cs_as_collation_internal(
													   tsql_find_collation_internal(coll_info_of_inputcollid.collname));
				if (NOT_FOUND == collidx_of_cs_as)
				{
					op->inputcollid = DEFAULT_COLLATION_OID;
					return node;
				}
				op->inputcollid = tsql_get_oid_from_collidx(collidx_of_cs_as);
			}
			else
			{
				/* If a collation is not specified, use the default one */
				op->inputcollid = DEFAULT_COLLATION_OID;
			}
		}

		if (OidIsValid(like_entry.like_oid) &&
			OidIsValid(coll_info_of_inputcollid.oid) &&
			coll_info_of_inputcollid.collateflags == 0x000e /* CS_AI  */ )
		{
			if (supported_AI_collation_for_like(coll_info_of_inputcollid.code_page))
				return transform_from_cs_ai_for_likenode(node, op, like_entry, coll_info_of_inputcollid);
			else
				ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("LIKE operator is not supported for \"%s\"", coll_info_of_inputcollid.collname)));
		}

		if (OidIsValid(like_entry.like_oid) &&
			OidIsValid(coll_info_of_inputcollid.oid) &&
			coll_info_of_inputcollid.collateflags == 0x000f /* CI_AI  */ )
		{
			if (supported_AI_collation_for_like(coll_info_of_inputcollid.code_page))
				return transform_from_ci_as_for_likenode(transform_likenode_for_AI(node, op), op, like_entry, coll_info_of_inputcollid);
			else
				ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("LIKE operator is not supported for \"%s\"", coll_info_of_inputcollid.collname)));
		}

		/* check if this is LIKE expr, and collation is CI_AS */
		if (OidIsValid(like_entry.like_oid) &&
			OidIsValid(coll_info_of_inputcollid.oid) &&
			coll_info_of_inputcollid.collateflags == 0x000d /* CI_AS  */ )
		{
			return transform_from_ci_as_for_likenode(node, op, like_entry, coll_info_of_inputcollid);
		}
	}
	return node;
}

Node *
pltsql_predicate_transformer(Node *expr)
{
	if (expr == NULL)
		return expr;

	if (IsA(expr, OpExpr))
	{
		/* Singleton predicate */
		return transform_likenode(expr);
	}
	else
	{
		/*
		 * Nonsingleton predicate, which could either a BoolExpr with a list
		 * of predicates or a simple List of predicates.
		 */
		ListCell   *lc;
		List	   *new_predicates = NIL;
		List	   *predicates;

		if (IsA(expr, List))
		{
			predicates = (List *) expr;
		}
		else if (IsA(expr, BoolExpr))
		{
			predicates = ((BoolExpr *)expr)->args;
		}
		else if (IsA(expr, FuncExpr))
		{
			/*
			 * This is performed even in the postgres dialect to handle
			 * babelfish CI_AS collations so that regexp operators can work
			 * inside plpgsql functions
			 */
			expr = expression_tree_mutator(expr, pgtsql_expression_tree_mutator, NULL);
			return transform_funcexpr(expr);
		}
		else if (IsA(expr, SubLink))
		{
			return expression_tree_mutator(expr, pgtsql_expression_tree_mutator, NULL);
		}
		else
			return expr;

		/*
		 * Process each predicate, and recursively process any nested
		 * predicate clauses of a toplevel predicate
		 */
		foreach(lc, predicates)
		{
			Node	   *qual = (Node *) lfirst(lc);

			/* For bool expr recall pltsql_predicate_transformer on its args */
			if (IsA(qual, BoolExpr))
			{
				new_predicates = lappend(new_predicates,
										 pltsql_predicate_transformer(qual));
			}
			else if (IsA(qual, OpExpr))
			{
				qual = transform_likenode(qual);
				new_predicates = lappend(new_predicates,
										 expression_tree_mutator(qual, pgtsql_expression_tree_mutator, NULL));
			}
			else
				new_predicates = lappend(new_predicates, qual);
		}

		if (IsA(expr, BoolExpr))
		{
			((BoolExpr *)expr)->args = new_predicates;
			return expr;
		}
		else
		{
			return (Node *) new_predicates;
		}
	}
}

static Node *
pgtsql_expression_tree_mutator(Node *node, void *context)
{
	if (NULL == node)
		return node;
	if (IsA(node, CaseExpr))
	{
		CaseExpr   *caseexpr = (CaseExpr *) node;

		if (caseexpr->arg != NULL)
			/* CASE expression WHEN... */
		{
			pltsql_predicate_transformer((Node *) caseexpr->arg);
		}
	}
	else if (IsA(node, CaseWhen))
		/* CASE WHEN expr */
	{
		CaseWhen   *casewhen = (CaseWhen *) node;

		pltsql_predicate_transformer((Node *) casewhen->expr);
	}

	/* Recurse through the operands of node */
	node = expression_tree_mutator(node, pgtsql_expression_tree_mutator, NULL);

	if (IsA(node, FuncExpr))
	{
		/*
		 * This is performed even in the postgres dialect to handle babelfish
		 * CI_AS collations so that regexp operators can work inside plpgsql
		 * functions
		 */
		node = transform_funcexpr(node);
	}
	else if (IsA(node, OpExpr))
	{
		/*
		 * Possibly a singleton LIKE predicate:  SELECT 'abc' LIKE 'ABC'; This
		 * is done even in the postgres dialect.
		 */
		node = transform_likenode(node);
	}

	return node;
}

Node *
pltsql_planner_node_transformer(PlannerInfo *root,
								Node *expr,
								int kind)
{
	/*
	 * Fall out quickly if expression is empty.
	 */
	if (expr == NULL)
		return NULL;

	if (EXPRKIND_TARGET == kind)
	{
		/*
		 * If expr is NOT a Boolean expression then recurse through its
		 * expresion tree
		 */
		return expression_tree_mutator(
									   expr,
									   pgtsql_expression_tree_mutator,
									   NULL);
	}
	return pltsql_predicate_transformer(expr);
}

static void
init_and_check_collation_callbacks(void)
{
	if (!collation_callbacks_ptr)
	{
		collation_callbacks **callbacks_ptr;

		callbacks_ptr = (collation_callbacks **) find_rendezvous_variable("collation_callbacks");
		collation_callbacks_ptr = *callbacks_ptr;

		/* collation_callbacks_ptr is still not initialised */
		if (!collation_callbacks_ptr)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("collation callbacks pointer is not initialised properly.")));
	}
}

Oid
tsql_get_server_collation_oid_internal(bool missingOk)
{
	if (OidIsValid(server_collation_oid))
		return server_collation_oid;

	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	server_collation_oid = (*collation_callbacks_ptr->get_server_collation_oid_internal) (missingOk);
	return server_collation_oid;
}

Datum
tsql_collation_list_internal(PG_FUNCTION_ARGS)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->collation_list_internal) (fcinfo);
}

Datum
tsql_is_collated_ci_as_internal(PG_FUNCTION_ARGS)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->is_collated_ci_as_internal) (fcinfo);
}

bytea *
tsql_tdscollationproperty_helper(const char *collationaname, const char *property)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->tdscollationproperty_helper) (collationaname, property);
}

int
tsql_collationproperty_helper(const char *collationaname, const char *property)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->collationproperty_helper) (collationaname, property);
}

bool
tsql_is_server_collation_CI_AS(void)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->is_server_collation_CI_AS) ();
}

bool
tsql_is_valid_server_collation_name(const char *collationname)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->is_valid_server_collation_name) (collationname);
}

int
tsql_find_locale(const char *locale)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->find_locale) (locale);
}

Oid
tsql_get_oid_from_collidx(int collidx)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->get_oid_from_collidx_internal) (collidx);
}

coll_info_t
tsql_lookup_collation_table_internal(Oid oid)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->lookup_collation_table_callback) (oid);
}

like_ilike_info_t
tsql_lookup_like_ilike_table_internal(Oid opno)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->lookup_like_ilike_table) (opno);
}

int
tsql_find_cs_as_collation_internal(int collidx)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->find_cs_as_collation_internal) (collidx);
}

int
tsql_find_collation_internal(const char *collation_name)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->find_collation_internal) (collation_name);
}


const char *
tsql_translate_bbf_collation_to_tsql_collation(const char *collname)
{
	/* Initialise collation callbacks */
	init_and_check_collation_callbacks();

	return (*collation_callbacks_ptr->translate_bbf_collation_to_tsql_collation) (collname);
}

bool
has_ilike_node_and_ci_as_coll(Node *expr)
{
	List	   *queue;

	if (expr == NULL)
		return false;

	queue = list_make1(expr);

	while (list_length(queue) > 0)
	{
		Node	   *predicate = (Node *) linitial(queue);

		queue = list_delete_first(queue);

		if (IsA(predicate, OpExpr))
		{
			Oid			inputcoll = ((OpExpr *) predicate)->inputcollid;

			/* Initialize collation callbacks */
			init_and_check_collation_callbacks();
			if ((*collation_callbacks_ptr->has_ilike_node) (predicate) &&
				DatumGetBool(DirectFunctionCall1Coll(tsql_is_collated_ci_as_internal, inputcoll, ObjectIdGetDatum(inputcoll))))
				return true;
		}
		else if (IsA(predicate, BoolExpr))
		{
			BoolExpr   *boolexpr = (BoolExpr *) predicate;

			queue = list_concat(queue, boolexpr->args);
		}
	}
	return false;
}
