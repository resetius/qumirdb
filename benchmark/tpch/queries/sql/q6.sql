select
	sum(l_extendedprice * l_discount) as revenue
from
	lineitem
where
	l_shipdate >= date '__Q6_DATE_S__'
	and l_shipdate < date '__Q6_DATE_S__' + interval '1' year
	and l_discount between __Q6_DISC_LOW__ and __Q6_DISC_HIGH__
	and l_quantity < __Q6_QUANTITY__;
