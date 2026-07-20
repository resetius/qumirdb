SELECT k, stddev_samp(a + b) AS s FROM t GROUP BY k;
