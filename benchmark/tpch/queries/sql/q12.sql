select
	l_shipmode,
	sum(case
		when o_orderpriority = '1-URGENT'
			or o_orderpriority = '2-HIGH'
			then 1
		else 0
	end) as high_line_count,
	sum(case
		when o_orderpriority <> '1-URGENT'
			and o_orderpriority <> '2-HIGH'
			then 1
		else 0
	end) as low_line_count
from
	orders,
	lineitem
where
	o_orderkey = l_orderkey
	and l_shipmode in ('__Q12_SHIPMODE1__', '__Q12_SHIPMODE2__')
	and l_commitdate < l_receiptdate
	and l_shipdate < l_commitdate
	and l_receiptdate >= date '__Q12_DATE_S__'
	and l_receiptdate < date '__Q12_DATE_S__' + interval '1' year
group by
	l_shipmode
order by
	l_shipmode;
