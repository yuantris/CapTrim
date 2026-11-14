//! Exact rational arithmetic for all timeline maths.
//!
//! Floating point accumulation drifts over a long timeline and eventually
//! breaks A/V sync. Every timestamp, frame rate, duration and speed in
//! CapTrim is therefore an integer ratio. Microseconds only appear at the
//! FFI/JSON boundary, via [`Rational::to_us`] / [`Rational::from_us`].

use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::hash::{Hash, Hasher};
use std::ops::{Add, Div, Mul, Sub};

#[derive(Clone, Copy, Debug, Serialize, Deserialize)]
pub struct Rational {
    pub num: i64,
    pub den: i64,
}

impl Rational {
    pub const ZERO: Rational = Rational { num: 0, den: 1 };
    pub const ONE: Rational = Rational { num: 1, den: 1 };

    /// Build a normalized ratio. Returns `None` for a zero denominator or for
    /// values that cannot be represented after reduction.
    pub fn new(num: i64, den: i64) -> Option<Self> {
        if den == 0 {
            return None;
        }
        let (n, d) = if den < 0 {
            (-(num as i128), -(den as i128))
        } else {
            (num as i128, den as i128)
        };
        let g = gcd(n.abs(), d.abs());
        let n = n / g;
        let d = d / g;
        if n > i64::MAX as i128 || n < i64::MIN as i128 || d > i64::MAX as i128 {
            return None;
        }
        Some(Self {
            num: n as i64,
            den: d as i64,
        })
    }

    /// Safe for untrusted JSON, where `serde` bypasses [`Rational::new`].
    pub fn is_valid(&self) -> bool {
        self.den > 0 && gcd(self.num.abs() as i128, self.den as i128) == 1
    }

    pub fn is_positive(&self) -> bool {
        self.den > 0 && self.num > 0
    }

    /// Truncating conversion to microseconds. Truncation is deliberate: a
    /// sub-microsecond rounding choice must never depend on float behaviour.
    pub fn to_us(&self) -> i64 {
        if self.den == 0 {
            return 0;
        }
        let v = (self.num as i128 * 1_000_000) / self.den as i128;
        i64::try_from(v).unwrap_or(if v < 0 { i64::MIN } else { i64::MAX })
    }

    pub fn from_us(us: i64) -> Self {
        Self::new(us, 1_000_000).unwrap_or(Self::ZERO)
    }

    pub fn reciprocal(&self) -> Option<Self> {
        Self::new(self.den, self.num)
    }

    pub fn to_f64(&self) -> f64 {
        if self.den == 0 {
            f64::NAN
        } else {
            self.num as f64 / self.den as f64
        }
    }

    /// Clamp into `[min, max]`, used where an out-of-range ratio would be a
    /// user-visible artefact (e.g. the playback slewing rate, where a large
    /// correction reads as a speed change).
    pub fn clamp_ratio(self, min: Self, max: Self) -> Self {
        if self < min {
            min
        } else if self > max {
            max
        } else {
            self
        }
    }

    fn raw(num: i128, den: i128) -> Self {
        if den == 0 {
            return Self::ZERO;
        }
        let (n, d) = if den < 0 { (-num, -den) } else { (num, den) };
        let g = gcd(n.abs(), d.abs());
        let n = n / g;
        let d = d / g;
        match (i64::try_from(n), i64::try_from(d)) {
            (Ok(n), Ok(d)) => Self { num: n, den: d },
            _ => Self::ZERO,
        }
    }

    /// `(a as i128 * b as i128) / c as i128`, saturating into a ratio.
    /// Used for speed conversions where naive i64 arithmetic would overflow.
    pub fn scaled_us_to_us(us: i64, mul: &Rational) -> i64 {
        if mul.den == 0 {
            return 0;
        }
        let v = us as i128 * mul.num as i128 / mul.den as i128;
        i64::try_from(v).unwrap_or(if v < 0 { i64::MIN } else { i64::MAX })
    }
}

fn gcd(mut a: i128, mut b: i128) -> i128 {
    if a == 0 {
        return if b == 0 { 1 } else { b };
    }
    while b != 0 {
        let r = a % b;
        a = b;
        b = r;
    }
    a.abs()
}

impl PartialEq for Rational {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other) == Ordering::Equal
    }
}
impl Eq for Rational {}

impl Ord for Rational {
    fn cmp(&self, other: &Self) -> Ordering {
        match (self.den == 0, other.den == 0) {
            (true, true) => Ordering::Equal,
            // A zero denominator means "invalid ratio". Such a value must not
            // compare equal to every valid ratio: that breaks transitivity of
            // `Eq` (and therefore `Hash`). Sort invalid ratios last instead.
            (true, false) => Ordering::Greater,
            (false, true) => Ordering::Less,
            (false, false) => {
                (self.num as i128 * other.den as i128).cmp(&(other.num as i128 * self.den as i128))
            }
        }
    }
}
impl PartialOrd for Rational {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Hash for Rational {
    fn hash<H: Hasher>(&self, state: &mut H) {
        // `PartialEq` compares by cross-multiplication, so `1/2 == 2/4`.
        // Hashing the raw fields would give those two different hashes and
        // silently break every `HashMap<Rational, _>` lookup in the engine.
        // Untrusted JSON can carry an unreduced ratio, so reduce here too.
        let (n, d) = if self.den == 0 {
            (0i64, 0i64)
        } else {
            // Normalize sign first: `-2/-4` must hash like `1/2`.
            let (num, den) = if self.den < 0 {
                (-(self.num as i128), -(self.den as i128))
            } else {
                (self.num as i128, self.den as i128)
            };
            let g = gcd(num.abs(), den);
            ((num / g) as i64, (den / g) as i64)
        };
        n.hash(state);
        d.hash(state);
    }
}

impl Add for Rational {
    type Output = Rational;
    fn add(self, rhs: Rational) -> Rational {
        Rational::raw(
            self.num as i128 * rhs.den as i128 + rhs.num as i128 * self.den as i128,
            self.den as i128 * rhs.den as i128,
        )
    }
}
impl Sub for Rational {
    type Output = Rational;
    fn sub(self, rhs: Rational) -> Rational {
        Rational::raw(
            self.num as i128 * rhs.den as i128 - rhs.num as i128 * self.den as i128,
            self.den as i128 * rhs.den as i128,
        )
    }
}
impl Mul for Rational {
    type Output = Rational;
    fn mul(self, rhs: Rational) -> Rational {
        Rational::raw(
            self.num as i128 * rhs.num as i128,
            self.den as i128 * rhs.den as i128,
        )
    }
}
impl Div for Rational {
    type Output = Rational;
    fn div(self, rhs: Rational) -> Rational {
        if rhs.num == 0 {
            return Rational::ZERO;
        }
        Rational::raw(
            self.num as i128 * rhs.den as i128,
            self.den as i128 * rhs.num as i128,
        )
    }
}

impl Default for Rational {
    fn default() -> Self {
        Self::ONE
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalizes_sign_and_gcd() {
        let r = Rational::new(30_000, 1001).unwrap();
        assert_eq!((r.num, r.den), (30_000, 1001));
        let r = Rational::new(4, -8).unwrap();
        assert_eq!((r.num, r.den), (-1, 2));
        assert!(Rational::new(1, 0).is_none());
    }

    #[test]
    fn microseconds_round_trip() {
        for us in [0i64, 1, 33_366, 1_000_000, 3_600_000_000, -123_456] {
            assert_eq!(Rational::from_us(us).to_us(), us);
        }
    }

    #[test]
    fn exact_comparison_without_floats() {
        let a = Rational::new(1, 3).unwrap();
        let b = Rational::new(333_333, 1_000_000).unwrap();
        assert!(a > b);
        assert_eq!(a * Rational::new(3, 1).unwrap(), Rational::ONE);
    }

    #[test]
    fn ntsc_frame_rate_frame_boundaries() {
        let fps = Rational::new(30_000, 1001).unwrap();
        let frame_duration = fps.reciprocal().unwrap();
        // 30000/1001 fps -> frame 1001 starts exactly at 1001 * 1001/30000 s
        let t = frame_duration * Rational::new(1001, 1).unwrap();
        assert_eq!(t.to_us(), 33_400_033);
    }

    #[test]
    fn deserialized_invalid_ratio_is_detected() {
        let bad: Rational = serde_json::from_str(r#"{"num":2,"den":0}"#).unwrap();
        assert!(!bad.is_valid());
        assert_eq!(bad.to_us(), 0);
    }

    #[test]
    fn equal_ratios_hash_equally() {
        use std::collections::hash_map::DefaultHasher;
        use std::collections::HashMap;

        fn h(r: &Rational) -> u64 {
            let mut s = DefaultHasher::new();
            r.hash(&mut s);
            s.finish()
        }

        let reduced = Rational::new(1, 2).unwrap();
        // Unreduced ratio as it can arrive from a hand-written draft JSON.
        let unreduced: Rational = serde_json::from_str(r#"{"num":2,"den":4}"#).unwrap();
        let negative: Rational = serde_json::from_str(r#"{"num":-2,"den":-4}"#).unwrap();

        assert_eq!(reduced, unreduced);
        assert_eq!(h(&reduced), h(&unreduced));
        assert_eq!(h(&reduced), h(&negative));

        let mut map: HashMap<Rational, &str> = HashMap::new();
        map.insert(reduced, "half");
        assert_eq!(map.get(&unreduced), Some(&"half"));
    }

    #[test]
    fn invalid_ratio_does_not_equal_everything() {
        let bad: Rational = serde_json::from_str(r#"{"num":2,"den":0}"#).unwrap();
        let bad2: Rational = serde_json::from_str(r#"{"num":9,"den":0}"#).unwrap();
        let half = Rational::new(1, 2).unwrap();
        let third = Rational::new(1, 3).unwrap();

        // Transitivity: bad == bad2 and half != third must stay consistent.
        assert_eq!(bad, bad2);
        assert_ne!(bad, half);
        assert_ne!(bad, third);
        assert_ne!(half, third);
    }
}
