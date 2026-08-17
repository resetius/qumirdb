// External modules: functions written in Kumir or Rust, compiled with the query
// and called from SQL. All three run on the JPL small-body dataset
// (unnumbered_asteroids, timestamps).
export const externalQueries = [
  {
    id: 'external-kumir-orbit',
    name: 'Kumir: orbital elements',
    sql: `-- Functions written in Kumir, compiled together with the query.
CREATE MODULE orbital
LANGUAGE kumir
AS $$
алг вещ orbit_perihelion_au(вещ a, e)
нач
    знач := a * (1.0 - e)
кон

алг вещ orbit_aphelion_au(вещ a, e)
нач
    знач := a * (1.0 + e)
кон

алг вещ orbit_period_days(вещ a)
нач
    | Третий закон Кеплера для гелиоцентрической орбиты, a в а.е.
    знач := 365.2568983 * sqrt(a * a * a)
кон

алг вещ orbit_mean_motion_deg_day(вещ a)
нач
    знач := 360.0 / orbit_period_days(a)
кон

алг лог orbit_is_neo(вещ a, e)
нач
    знач := orbit_perihelion_au(a, e) < 1.3
кон
$$;

SELECT
    a.designation,
    t.timestamp AS timestamp,
    a.m + orbit_mean_motion_deg_day(a.a) * (t.timestamp - a.epoch) AS mean_anomaly,
    orbit_perihelion_au(a.a, a.e) AS perihelion_au,
    orbit_aphelion_au(a.a, a.e) AS aphelion_au
FROM unnumbered_asteroids a
CROSS JOIN timestamps t
WHERE
    orbit_is_neo(a.a, a.e)
    AND a.e < 0.1
    AND t.timestamp >= 61564
    AND t.timestamp < 61565
LIMIT 10;
`
  },
  {
    id: 'external-rust-orbit',
    name: 'Rust: orbital elements',
    sql: `-- The same functions in Rust. Each one is exported by name and bound to a
-- SQL signature with CREATE FUNCTION.
CREATE MODULE orbital
LANGUAGE rust
AS $$
#[no_mangle]
pub extern "C" fn orbit_perihelion_au(a: f64, e: f64) -> f64 {
    a * (1.0 - e)
}

#[no_mangle]
pub extern "C" fn orbit_aphelion_au(a: f64, e: f64) -> f64 {
    a * (1.0 + e)
}

#[no_mangle]
pub extern "C" fn orbit_period_days(a: f64) -> f64 {
    // Kepler's third law for a heliocentric orbit, a in AU.
    365.2568983 * (a * a * a).sqrt()
}

#[no_mangle]
pub extern "C" fn orbit_mean_motion_deg_day(a: f64) -> f64 {
    360.0 / orbit_period_days(a)
}

#[no_mangle]
pub extern "C" fn orbit_is_neo(a: f64, e: f64) -> bool {
    orbit_perihelion_au(a, e) < 1.3
}
$$;

CREATE FUNCTION orbit_perihelion_au(a DOUBLE, e DOUBLE)
RETURNS DOUBLE
SET MODULE TO orbital
SET SYMBOL TO orbit_perihelion_au;

CREATE FUNCTION orbit_aphelion_au(a DOUBLE, e DOUBLE)
RETURNS DOUBLE
SET MODULE TO orbital
SET SYMBOL TO orbit_aphelion_au;

CREATE FUNCTION orbit_period_days(a DOUBLE)
RETURNS DOUBLE
SET MODULE TO orbital
SET SYMBOL TO orbit_period_days;

CREATE FUNCTION orbit_mean_motion_deg_day(a DOUBLE)
RETURNS DOUBLE
SET MODULE TO orbital
SET SYMBOL TO orbit_mean_motion_deg_day;

CREATE FUNCTION orbit_is_neo(a DOUBLE, e DOUBLE)
RETURNS BOOL
SET MODULE TO orbital
SET SYMBOL TO orbit_is_neo;

SELECT
    designation,
    epoch,
    a,
    e,
    orbit_perihelion_au(a, e) AS perihelion_au,
    orbit_aphelion_au(a, e) AS aphelion_au,
    orbit_period_days(a) AS period_days,
    orbit_mean_motion_deg_day(a) AS mean_motion_deg_day
FROM unnumbered_asteroids
LIMIT 20;
`
  },
  {
    id: 'external-rust-position',
    name: 'Rust: heliocentric position',
    sql: `-- A function returning a struct: Kepler's equation is solved per row and the
-- three coordinates land in three columns.
CREATE MODULE orbital
LANGUAGE rust
AS $$
use std::f64::consts::PI;

const GAUSSIAN_K: f64 = 0.01720209895; // rad/day

#[repr(C)]
pub struct Vec3 {
    pub x: f64,
    pub y: f64,
    pub z: f64,
}

#[inline(always)]
fn deg_to_rad(x: f64) -> f64 {
    x * PI / 180.0
}

#[inline(always)]
fn normalize_angle(x: f64) -> f64 {
    x.rem_euclid(2.0 * PI)
}

#[inline(always)]
fn mean_motion_rad_day(a: f64) -> f64 {
    GAUSSIAN_K / (a * a * a).sqrt()
}

#[inline(always)]
fn solve_kepler(m: f64, e: f64) -> f64 {
    let m = normalize_angle(m);

    let mut eccentric_anomaly = if e < 0.8 { m } else { PI };

    let mut n = 0;
    while n < 20 {
        let f = eccentric_anomaly - e * eccentric_anomaly.sin() - m;
        let fp = 1.0 - e * eccentric_anomaly.cos();

        let delta = f / fp;
        eccentric_anomaly -= delta;

        if delta.abs() < 1.0e-13 {
            break;
        }

        n += 1;
    }

    eccentric_anomaly
}

#[no_mangle]
pub extern "C" fn orbit_is_neo(a: f64, e: f64) -> bool {
    // Near-Earth object criterion: perihelion < 1.3 AU.
    a * (1.0 - e) < 1.3
}

#[no_mangle]
pub extern "C" fn orbit_position(
    a: f64,
    e: f64,
    inclination_deg: f64,
    arg_perihelion_deg: f64,
    ascending_node_deg: f64,
    mean_anomaly_epoch_deg: f64,
    epoch: f64,
    timestamp: f64,
) -> Vec3 {
    let inclination = deg_to_rad(inclination_deg);
    let arg_perihelion = deg_to_rad(arg_perihelion_deg);
    let ascending_node = deg_to_rad(ascending_node_deg);

    let m0 = deg_to_rad(mean_anomaly_epoch_deg);
    let m = m0 + mean_motion_rad_day(a) * (timestamp - epoch);

    let eccentric_anomaly = solve_kepler(m, e);

    // Coordinates in the orbital plane, AU.
    let x_orbit = a * (eccentric_anomaly.cos() - e);
    let y_orbit = a * (1.0 - e * e).sqrt() * eccentric_anomaly.sin();

    let cos_w = arg_perihelion.cos();
    let sin_w = arg_perihelion.sin();

    let cos_i = inclination.cos();
    let sin_i = inclination.sin();

    let cos_node = ascending_node.cos();
    let sin_node = ascending_node.sin();

    // Rotate from the orbital plane to heliocentric ecliptic coordinates.
    let x1 = cos_w * x_orbit - sin_w * y_orbit;
    let y1 = sin_w * x_orbit + cos_w * y_orbit;

    let x2 = x1;
    let y2 = cos_i * y1;
    let z2 = sin_i * y1;

    Vec3 {
        x: cos_node * x2 - sin_node * y2,
        y: sin_node * x2 + cos_node * y2,
        z: z2,
    }
}
$$;

CREATE FUNCTION orbit_is_neo(a DOUBLE, e DOUBLE)
RETURNS BOOL
SET MODULE TO orbital;

CREATE FUNCTION orbit_position(
    a DOUBLE,
    e DOUBLE,
    i DOUBLE,
    w DOUBLE,
    node DOUBLE,
    m DOUBLE,
    epoch DOUBLE,
    timestamp DOUBLE
)
RETURNS (DOUBLE, DOUBLE, DOUBLE)
SET MODULE TO orbital;

SELECT
    a.designation,
    t.timestamp,
    orbit_position(a.a, a.e, a.i, a.w, a.node, a.m, a.epoch, t.timestamp) AS (x, y, z)
FROM unnumbered_asteroids a
CROSS JOIN timestamps t
WHERE orbit_is_neo(a.a, a.e)
    AND a.e < 0.3
    AND t.timestamp >= 61540
    AND t.timestamp < 61565
LIMIT 100;
`
  }
];
