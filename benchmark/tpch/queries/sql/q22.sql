select
	cntrycode,
	count(*) as numcust,
	sum(c_acctbal) as totacctbal
from
	(
		select
			substring(c_phone from 1 for 2) as cntrycode,
			c_acctbal
		from
			customer
		where
			substring(c_phone from 1 for 2) in
				('__Q22_CC1__', '__Q22_CC2__', '__Q22_CC3__', '__Q22_CC4__', '__Q22_CC5__', '__Q22_CC6__', '__Q22_CC7__')
			and c_acctbal > (
				select
					avg(c_acctbal)
				from
					customer
				where
					c_acctbal > 0.00
					and substring(c_phone from 1 for 2) in
						('__Q22_CC1__', '__Q22_CC2__', '__Q22_CC3__', '__Q22_CC4__', '__Q22_CC5__', '__Q22_CC6__', '__Q22_CC7__')
			)
			and not exists (
				select
					*
				from
					orders
				where
					o_custkey = c_custkey
			)
	) as custsale
group by
	cntrycode
order by
	cntrycode;
