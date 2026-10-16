//! A small JSON value with a parser and serializer, enough for MCP and the
//! sync protocol. Objects keep their key order.

use std::fmt::Write as _;

#[derive(Debug, Clone, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<Json>),
    Obj(Vec<(String, Json)>),
}

impl Json {
    pub fn obj() -> Json {
        Json::Obj(Vec::new())
    }

    /// Adds or replaces a key; returns self for chaining.
    pub fn with(mut self, key: &str, value: impl Into<Json>) -> Json {
        self.set(key, value);
        self
    }

    pub fn set(&mut self, key: &str, value: impl Into<Json>) {
        if let Json::Obj(items) = self {
            let value = value.into();
            if let Some(slot) = items.iter_mut().find(|(k, _)| k == key) {
                slot.1 = value;
            } else {
                items.push((key.to_string(), value));
            }
        }
    }

    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(items) => items.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_f64(&self) -> Option<f64> {
        match self {
            Json::Num(n) => Some(*n),
            _ => None,
        }
    }

    pub fn as_u64(&self) -> Option<u64> {
        self.as_f64().filter(|n| *n >= 0.0 && n.fract() == 0.0).map(|n| n as u64)
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Json::Bool(b) => Some(*b),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[Json]> {
        match self {
            Json::Arr(a) => Some(a),
            _ => None,
        }
    }

    pub fn is_null(&self) -> bool {
        matches!(self, Json::Null)
    }

    /// `obj.str_or("key", "")`: a string field or a default.
    pub fn str_or<'a>(&'a self, key: &str, default: &'a str) -> &'a str {
        self.get(key).and_then(Json::as_str).unwrap_or(default)
    }

    pub fn bool_or(&self, key: &str, default: bool) -> bool {
        self.get(key).and_then(Json::as_bool).unwrap_or(default)
    }

    pub fn dump(&self) -> String {
        let mut out = String::new();
        write_value(self, &mut out);
        out
    }

    pub fn parse(text: &str) -> Result<Json, String> {
        let mut p = Parser { s: text.as_bytes(), i: 0, depth: 0 };
        p.ws();
        let v = p.value()?;
        p.ws();
        if p.i != p.s.len() {
            return Err(format!("unexpected trailing data at byte {}", p.i));
        }
        Ok(v)
    }
}

impl From<&str> for Json {
    fn from(s: &str) -> Json {
        Json::Str(s.to_string())
    }
}
impl From<String> for Json {
    fn from(s: String) -> Json {
        Json::Str(s)
    }
}
impl From<&String> for Json {
    fn from(s: &String) -> Json {
        Json::Str(s.clone())
    }
}
impl From<bool> for Json {
    fn from(b: bool) -> Json {
        Json::Bool(b)
    }
}
impl From<f64> for Json {
    fn from(n: f64) -> Json {
        Json::Num(n)
    }
}
impl From<u64> for Json {
    fn from(n: u64) -> Json {
        Json::Num(n as f64)
    }
}
impl From<u16> for Json {
    fn from(n: u16) -> Json {
        Json::Num(n as f64)
    }
}
impl From<usize> for Json {
    fn from(n: usize) -> Json {
        Json::Num(n as f64)
    }
}
impl From<i64> for Json {
    fn from(n: i64) -> Json {
        Json::Num(n as f64)
    }
}
impl From<Vec<Json>> for Json {
    fn from(v: Vec<Json>) -> Json {
        Json::Arr(v)
    }
}
impl<T: Into<Json>> From<Option<T>> for Json {
    fn from(v: Option<T>) -> Json {
        v.map(Into::into).unwrap_or(Json::Null)
    }
}

pub fn write_string(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

fn write_value(v: &Json, out: &mut String) {
    match v {
        Json::Null => out.push_str("null"),
        Json::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Json::Num(n) => {
            if !n.is_finite() {
                out.push_str("null");
            } else if n.fract() == 0.0 && n.abs() < 9.007e15 {
                let _ = write!(out, "{}", *n as i64);
            } else {
                let _ = write!(out, "{n}");
            }
        }
        Json::Str(s) => write_string(s, out),
        Json::Arr(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_value(item, out);
            }
            out.push(']');
        }
        Json::Obj(items) => {
            out.push('{');
            for (i, (k, item)) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_string(k, out);
                out.push(':');
                write_value(item, out);
            }
            out.push('}');
        }
    }
}

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
    depth: usize,
}

const MAX_DEPTH: usize = 128;

impl Parser<'_> {
    fn ws(&mut self) {
        while self.i < self.s.len() && matches!(self.s[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }

    fn err<T>(&self, what: &str) -> Result<T, String> {
        Err(format!("{what} at byte {}", self.i))
    }

    fn lit(&mut self, word: &str, v: Json) -> Result<Json, String> {
        if self.s[self.i..].starts_with(word.as_bytes()) {
            self.i += word.len();
            Ok(v)
        } else {
            self.err("invalid literal")
        }
    }

    fn value(&mut self) -> Result<Json, String> {
        match self.s.get(self.i) {
            None => self.err("unexpected end"),
            Some(b'n') => self.lit("null", Json::Null),
            Some(b't') => self.lit("true", Json::Bool(true)),
            Some(b'f') => self.lit("false", Json::Bool(false)),
            Some(b'"') => Ok(Json::Str(self.string()?)),
            Some(b'[') => {
                self.enter()?;
                self.i += 1;
                let mut items = Vec::new();
                self.ws();
                if self.s.get(self.i) == Some(&b']') {
                    self.i += 1;
                    self.depth -= 1;
                    return Ok(Json::Arr(items));
                }
                loop {
                    self.ws();
                    items.push(self.value()?);
                    self.ws();
                    match self.s.get(self.i) {
                        Some(b',') => self.i += 1,
                        Some(b']') => {
                            self.i += 1;
                            break;
                        }
                        _ => return self.err("expected , or ]"),
                    }
                }
                self.depth -= 1;
                Ok(Json::Arr(items))
            }
            Some(b'{') => {
                self.enter()?;
                self.i += 1;
                let mut items: Vec<(String, Json)> = Vec::new();
                self.ws();
                if self.s.get(self.i) == Some(&b'}') {
                    self.i += 1;
                    self.depth -= 1;
                    return Ok(Json::Obj(items));
                }
                loop {
                    self.ws();
                    if self.s.get(self.i) != Some(&b'"') {
                        return self.err("expected a key");
                    }
                    let key = self.string()?;
                    self.ws();
                    if self.s.get(self.i) != Some(&b':') {
                        return self.err("expected :");
                    }
                    self.i += 1;
                    self.ws();
                    let v = self.value()?;
                    if let Some(slot) = items.iter_mut().find(|(k, _)| *k == key) {
                        slot.1 = v; // last duplicate wins, like most parsers
                    } else {
                        items.push((key, v));
                    }
                    self.ws();
                    match self.s.get(self.i) {
                        Some(b',') => self.i += 1,
                        Some(b'}') => {
                            self.i += 1;
                            break;
                        }
                        _ => return self.err("expected , or }"),
                    }
                }
                self.depth -= 1;
                Ok(Json::Obj(items))
            }
            Some(b'-' | b'0'..=b'9') => self.number(),
            Some(_) => self.err("unexpected character"),
        }
    }

    fn enter(&mut self) -> Result<(), String> {
        self.depth += 1;
        if self.depth > MAX_DEPTH {
            return self.err("nested too deeply");
        }
        Ok(())
    }

    fn number(&mut self) -> Result<Json, String> {
        let start = self.i;
        if self.s[self.i] == b'-' {
            self.i += 1;
        }
        let digits = |p: &mut Self| {
            let s = p.i;
            while p.i < p.s.len() && p.s[p.i].is_ascii_digit() {
                p.i += 1;
            }
            p.i - s
        };
        if digits(self) == 0 {
            return self.err("invalid number");
        }
        if self.s.get(self.i) == Some(&b'.') {
            self.i += 1;
            if digits(self) == 0 {
                return self.err("invalid number");
            }
        }
        if matches!(self.s.get(self.i), Some(b'e' | b'E')) {
            self.i += 1;
            if matches!(self.s.get(self.i), Some(b'+' | b'-')) {
                self.i += 1;
            }
            if digits(self) == 0 {
                return self.err("invalid number");
            }
        }
        let text = std::str::from_utf8(&self.s[start..self.i]).map_err(|e| e.to_string())?;
        text.parse::<f64>().map(Json::Num).map_err(|e| e.to_string())
    }

    fn hex4(&mut self) -> Result<u32, String> {
        let h = self.s.get(self.i..self.i + 4).ok_or("truncated \\u escape")?;
        let v = u32::from_str_radix(std::str::from_utf8(h).map_err(|e| e.to_string())?, 16)
            .map_err(|_| format!("invalid \\u escape at byte {}", self.i))?;
        self.i += 4;
        Ok(v)
    }

    fn string(&mut self) -> Result<String, String> {
        self.i += 1; // opening quote
        let mut out = String::new();
        loop {
            let start = self.i;
            while self.i < self.s.len() && self.s[self.i] != b'"' && self.s[self.i] != b'\\' && self.s[self.i] >= 0x20 {
                self.i += 1;
            }
            out.push_str(std::str::from_utf8(&self.s[start..self.i]).map_err(|_| "invalid UTF-8".to_string())?);
            match self.s.get(self.i) {
                None => return self.err("unterminated string"),
                Some(b'"') => {
                    self.i += 1;
                    return Ok(out);
                }
                Some(b'\\') => {
                    self.i += 1;
                    let c = *self.s.get(self.i).ok_or("unterminated escape")?;
                    self.i += 1;
                    match c {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let mut cp = self.hex4()?;
                            if (0xD800..0xDC00).contains(&cp) {
                                if self.s.get(self.i..self.i + 2) != Some(b"\\u") {
                                    return self.err("lone surrogate");
                                }
                                self.i += 2;
                                let lo = self.hex4()?;
                                if !(0xDC00..0xE000).contains(&lo) {
                                    return self.err("invalid surrogate pair");
                                }
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else if (0xDC00..0xE000).contains(&cp) {
                                return self.err("lone surrogate");
                            }
                            out.push(char::from_u32(cp).ok_or("invalid code point")?);
                        }
                        _ => return self.err("invalid escape"),
                    }
                }
                Some(_) => return self.err("control character in string"),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_mcp_message() {
        let v = Json::parse(
            r#" {"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"share_start","arguments":{"folder":"C:\\Users\\me","write":true,"port":8740.0,"x":null,"list":[1,-2.5e3,"a"]}}} "#,
        )
        .unwrap();
        assert_eq!(v.get("id").unwrap().as_u64(), Some(7));
        let args = v.get("params").unwrap().get("arguments").unwrap();
        assert_eq!(args.str_or("folder", ""), "C:\\Users\\me");
        assert!(args.bool_or("write", false));
        assert_eq!(args.get("port").unwrap().as_u64(), Some(8740));
        assert!(args.get("x").unwrap().is_null());
        assert_eq!(args.get("list").unwrap().as_array().unwrap()[1], Json::Num(-2500.0));
    }

    #[test]
    fn unicode_escapes() {
        let v = Json::parse(r#""\u00e9\ud83d\ude00\n\"\/""#).unwrap();
        assert_eq!(v.as_str().unwrap(), "é😀\n\"/");
        assert!(Json::parse(r#""\ud83d""#).is_err());
        assert!(Json::parse(r#""\udc00""#).is_err());
        assert_eq!(Json::parse("\"日本\"").unwrap().as_str().unwrap(), "日本");
    }

    #[test]
    fn rejects_bad_input() {
        for bad in ["", "{", "[1,]", "{\"a\" 1}", "01x", "tru", "\"a", "{\"a\":1}x", "-", "1.", "\"\u{1}\"", "[1 2]"] {
            assert!(Json::parse(bad).is_err(), "accepted {bad:?}");
        }
        let deep = "[".repeat(200) + &"]".repeat(200);
        assert!(Json::parse(&deep).is_err());
    }

    #[test]
    fn round_trip() {
        let v = Json::obj()
            .with("s", "a\"b\\c\n\u{1}")
            .with("n", 3u64)
            .with("f", 1.5)
            .with("b", false)
            .with("z", Json::Null)
            .with("a", vec![Json::from("x"), Json::obj()]);
        let text = v.dump();
        assert_eq!(text, r#"{"s":"a\"b\\c\n\u0001","n":3,"f":1.5,"b":false,"z":null,"a":["x",{}]}"#);
        assert_eq!(Json::parse(&text).unwrap(), v);
    }
}
