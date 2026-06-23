select
	l_orderkey,
	sum(l_extendedprice * (1 - l_discount)) as revenue,
	o_orderdate,
	o_shippriority
from
	customer,
	orders,
	lineitem
where
	c_mktsegment = '__Q3_SEGMENT__'
	and c_custkey = o_custkey
	and l_orderkey = o_orderkey
	and o_orderdate < date '__Q3_DATE_S__'
	and l_shipdate > date '__Q3_DATE_S__'
group by
	l_orderkey,
	o_orderdate,
	o_shippriority
order by
	revenue desc,
	o_orderdate
limit 10;
