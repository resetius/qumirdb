(rel sort
  (rel aggregate
    (rel join
      (rel filter
        (rel source "__ORDERS__")
        (: (&& (>= o_orderdate (: __Q4_DATE_LOW__ i32))
               (< o_orderdate  (: __Q4_DATE_HIGH__ i32))) u8))
      (rel filter
        (rel source "__LINEITEM__")
        (: (< l_commitdate l_receiptdate) u8))
      ((o_orderkey l_orderkey)) (left_semi))
    (keys o_orderpriority)
    (agg order_count count))
  (o_orderpriority asc nulls-default))
