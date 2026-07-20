SELECT a, b, grouping(a) ga, grouping(b) gb, count(*) c
FROM t
GROUP BY ROLLUP(a, b);
