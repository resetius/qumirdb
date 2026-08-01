SELECT sum(a), sum(b) FROM t GROUP BY c HAVING sum(a) > 1 AND sum(b) > 2;
