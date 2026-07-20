SELECT grouping(c)
FROM t
GROUP BY ROLLUP(a, b);
