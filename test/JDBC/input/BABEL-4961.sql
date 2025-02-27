-- there's invisible space in front on procedure name
create procedure [​babel_4961_proc1] as select 'with invisible space';
GO

-- no invisible space in front
create procedure [babel_4961_proc1] as select 'without invisible space';
GO

create procedure [  babel_4961_proc2] as select 2;
GO

create table babel_4961_tbl (a int);
GO

insert into babel_4961_tbl values (1), (2), (3);
GO

create procedure babel_4961_proc3 @param int
as
    select * from babel_4961_tbl where a = @param;
go

exec [​babel_4961_proc1]
GO

exec [dbo].[​babel_4961_proc1]
GO

exec [dbo].​babel_4961_proc1
GO

exec dbo.[​babel_4961_proc1]
GO

exec [babel_4961_proc1]
GO

exec [dbo].[babel_4961_proc1]
GO

exec [dbo].babel_4961_proc1
GO

exec dbo.[babel_4961_proc1]
GO

exec [  babel_4961_proc2]
GO

exec [dbo].[  babel_4961_proc2]
GO

-- invisble space in front
-- should throw an error that proc does not exist
exec [​babel_4961_proc3] 3
GO

exec [babel_4961_proc3] 1
GO

exec [dbo].[babel_4961_proc3] 1
GO

-- dependent objects
-- should not crash the server
CREATE PROCEDURE [dbo].test_depend_proc AS EXEC [dbo].[​babel_4961_proc1];
GO

-- 
CREATE FUNCTION test_depend_func(@param int) RETURNS INT AS 
BEGIN 
    EXEC [dbo].[​babel_4961_proc1]; 
    RETURN (@i) 
END;
GO

DROP PROCEDURE [dbo].test_depend_proc
GO

DROP FUNCTION test_depend_func(int);
GO

drop procedure [​babel_4961_proc1];
GO

drop procedure [babel_4961_proc1];
GO

drop procedure [  babel_4961_proc2]
GO

drop table babel_4961_tbl;
GO

drop procedure babel_4961_proc3
GO