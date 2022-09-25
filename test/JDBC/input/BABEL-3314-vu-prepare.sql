CREATE VIEW BABEL_3314_vu_prepare_v1 AS (SELECT CONVERT(DATETIME, '10/1/22'));
GO

CREATE VIEW BABEL_3314_vu_prepare_v2 AS (SELECT CONVERT(DATE, '10/1/22'));
GO


CREATE PROCEDURE BABEL_3314_vu_prepare_p1 AS (SELECT CONVERT(DATETIME, '10/1/22'));
GO

CREATE PROCEDURE BABEL_3314_vu_prepare_p2 AS (SELECT CONVERT(DATE, '10/1/22'));
GO

CREATE FUNCTION BABEL_3314_vu_prepare_f1()
RETURNS DATETIME AS
BEGIN
RETURN (SELECT CONVERT(DATETIME, '10/1/22'));
END
GO

CREATE FUNCTION BABEL_3314_vu_prepare_f2()
RETURNS DATE AS
BEGIN
RETURN (SELECT CONVERT(DATE, '10/1/22'));
END
GO
