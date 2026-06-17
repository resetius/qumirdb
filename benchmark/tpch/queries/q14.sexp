(rel project
  (rel aggregate
    (rel project
      (rel join
        (rel filter (rel source "__LINEITEM__")
          (: (&& (>= l_shipdate (: __Q14_DATE_LOW__ i32)) (< l_shipdate (: __Q14_DATE_HIGH__ i32))) u8))
        (rel source "__PART__") ((l_partkey p_partkey)) (inner))
      (disc_price (* l_extendedprice (- (: 1.0 f64) l_discount)))
      (promo_disc_price (if (call qdb_string_view_sql_like p_type "PROMO%") (* l_extendedprice (- (: 1.0 f64) l_discount)) (: 0.0 f64)))
      (dummy (: 1 i64)))
    (keys dummy)
    (agg sum_promo sum promo_disc_price)
    (agg sum_total sum disc_price))
  (promo_revenue (/ (* (: 100.0 f64) sum_promo) sum_total)))
