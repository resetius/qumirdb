SELECT ca_zip FROM customer_address
WHERE substr(ca_zip, 1, 5) IN ('89436', '30868');
