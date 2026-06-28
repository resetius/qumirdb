(rel sort
  (rel aggregate
    (rel project
      (rel join
        (rel source "__ORDERS__")
        (rel filter (rel source "__LINEITEM__")
          (: (&& (&& (|| (== l_shipmode "__Q12_SHIPMODE1__") (== l_shipmode "__Q12_SHIPMODE2__"))
                     (&& (< l_commitdate l_receiptdate)
                         (< l_shipdate l_commitdate)))
                 (&& (>= l_receiptdate (: __Q12_DATE_LOW__ i32))
                     (< l_receiptdate (: __Q12_DATE_HIGH__ i32)))) u8))
        ((o_orderkey l_orderkey)) (inner))
      (l_shipmode l_shipmode)
      (is_high (if (|| (== o_orderpriority "1-URGENT") (== o_orderpriority "2-HIGH")) (: 1 i64) (: 0 i64)))
      (is_low  (if (&& (!= o_orderpriority "1-URGENT") (!= o_orderpriority "2-HIGH")) (: 1 i64) (: 0 i64))))
    (keys l_shipmode)
    (agg high_line_count sum is_high)
    (agg low_line_count  sum is_low))
  (l_shipmode asc nulls-default))
