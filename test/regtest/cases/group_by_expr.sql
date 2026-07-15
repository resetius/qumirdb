SELECT a + b AS k, sum(c) AS s FROM t GROUP BY a + b HAVING a + b > 5;
