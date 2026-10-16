//! Just enough JSON to edit config files without dependencies: parse, keep key
//! order, change a few keys, and write it back the way `JSON.stringify(v, null, 2)`
//! does (the format Claude's apps write themselves).

#[derive(Debug, Clone, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    /// Kept as written, so numbers round-trip exactly.
    Number(String),
    String(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

impl Json {
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Object(entries) => entries.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn get_mut(&mut self, key: &str) -> Option<&mut Json> {
        match self {
            Json::Object(entries) => entries.iter_mut().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    /// Sets `key` on an object (in place if present, else appended). No-op on non-objects.
    pub fn set(&mut self, key: &str, value: Json) {
        if let Json::Object(entries) = self {
            if let Some(slot) = entries.iter_mut().find(|(k, _)| k == key) {
                slot.1 = value;
            } else {
                entries.push((key.to_string(), value));
            }
        }
    }

    /// Removes `key` from an object; true if it was there.
    pub fn remove(&mut self, key: &str) -> bool {
        if let Json::Object(entries) = self {
            let before = entries.len();
            entries.retain(|(k, _)| k != key);
            return entries.len() != before;
        }
        false
    }

    pub fn pretty(&self) -> String {
        let mut out = String::new();
        write_value(self, 0, &mut out);
        out
    }
}

pub fn quote(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn indent(level: usize, out: &mut String) {
    for _ in 0..level {
        out.push_str("  ");
    }
}

fn write_value(v: &Json, level: usize, out: &mut String) {
    match v {
        Json::Null => out.push_str("null"),
        Json::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Json::Number(n) => out.push_str(n),
        Json::String(s) => out.push_str(&quote(s)),
        Json::Array(items) if items.is_empty() => out.push_str("[]"),
        Json::Object(entries) if entries.is_empty() => out.push_str("{}"),
        Json::Array(items) => {
            out.push_str("[\n");
            for (i, item) in items.iter().enumerate() {
                indent(level + 1, out);
                write_value(item, level + 1, out);
                out.push_str(if i + 1 < items.len() { ",\n" } else { "\n" });
            }
            indent(level, out);
            out.push(']');
        }
        Json::Object(entries) => {
            out.push_str("{\n");
            for (i, (k, item)) in entries.iter().enumerate() {
                indent(level + 1, out);
                out.push_str(&quote(k));
                out.push_str(": ");
                write_value(item, level + 1, out);
                out.push_str(if i + 1 < entries.len() { ",\n" } else { "\n" });
            }
            indent(level, out);
            out.push('}');
        }
    }
}

pub fn parse(text: &str) -> Result<Json, String> {
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let mut p = Parser { s: text.as_bytes(), i: 0, depth: 0 };
    p.ws();
    let v = p.value()?;
    p.ws();
    if p.i != p.s.len() {
        return Err(p.err("unexpected text after the end"));
    }
    Ok(v)
}

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
    depth: usize,
}

impl Parser<'_> {
    fn err(&self, what: &str) -> String {
        let line = self.s[..self.i.min(self.s.len())].iter().filter(|&&b| b == b'\n').count() + 1;
        format!("{what} on line {line}")
    }

    fn ws(&mut self) {
        while self.i < self.s.len() && matches!(self.s[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }

    fn eat(&mut self, word: &str) -> bool {
        if self.s[self.i..].starts_with(word.as_bytes()) {
            self.i += word.len();
            true
        } else {
            false
        }
    }

    fn value(&mut self) -> Result<Json, String> {
        self.depth += 1;
        if self.depth > 512 {
            return Err(self.err("nested too deeply"));
        }
        let v = match self.s.get(self.i) {
            None => Err(self.err("unexpected end")),
            Some(b'{') => self.object(),
            Some(b'[') => self.array(),
            Some(b'"') => self.string().map(Json::String),
            Some(b't') if self.eat("true") => Ok(Json::Bool(true)),
            Some(b'f') if self.eat("false") => Ok(Json::Bool(false)),
            Some(b'n') if self.eat("null") => Ok(Json::Null),
            Some(c) if *c == b'-' || c.is_ascii_digit() => self.number(),
            Some(_) => Err(self.err("unexpected character")),
        };
        self.depth -= 1;
        v
    }

    fn number(&mut self) -> Result<Json, String> {
        let start = self.i;
        while self.i < self.s.len() && matches!(self.s[self.i], b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9') {
            self.i += 1;
        }
        let raw = std::str::from_utf8(&self.s[start..self.i]).map_err(|_| self.err("bad number"))?;
        if raw.parse::<f64>().is_err() {
            return Err(self.err("bad number"));
        }
        Ok(Json::Number(raw.to_string()))
    }

    fn hex4(&mut self) -> Result<u32, String> {
        let digits = self.s.get(self.i..self.i + 4).ok_or_else(|| self.err("bad \\u escape"))?;
        let text = std::str::from_utf8(digits).map_err(|_| self.err("bad \\u escape"))?;
        let n = u32::from_str_radix(text, 16).map_err(|_| self.err("bad \\u escape"))?;
        self.i += 4;
        Ok(n)
    }

    fn string(&mut self) -> Result<String, String> {
        self.i += 1; // opening quote
        let mut out: Vec<u8> = Vec::new();
        loop {
            let Some(&c) = self.s.get(self.i) else {
                return Err(self.err("unterminated string"));
            };
            self.i += 1;
            match c {
                b'"' => break,
                b'\\' => {
                    let Some(&e) = self.s.get(self.i) else {
                        return Err(self.err("unterminated string"));
                    };
                    self.i += 1;
                    let ch = match e {
                        b'"' => '"',
                        b'\\' => '\\',
                        b'/' => '/',
                        b'b' => '\u{8}',
                        b'f' => '\u{c}',
                        b'n' => '\n',
                        b'r' => '\r',
                        b't' => '\t',
                        b'u' => {
                            let mut n = self.hex4()?;
                            if (0xd800..0xdc00).contains(&n) && self.eat("\\u") {
                                let low = self.hex4()?;
                                n = 0x10000 + ((n - 0xd800) << 10) + (low.wrapping_sub(0xdc00) & 0x3ff);
                            }
                            char::from_u32(n).unwrap_or('\u{fffd}')
                        }
                        _ => return Err(self.err("bad escape")),
                    };
                    let mut buf = [0u8; 4];
                    out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
                }
                c => out.push(c),
            }
        }
        String::from_utf8(out).map_err(|_| self.err("string isn't UTF-8"))
    }

    fn array(&mut self) -> Result<Json, String> {
        self.i += 1;
        let mut items = Vec::new();
        self.ws();
        if self.eat("]") {
            return Ok(Json::Array(items));
        }
        loop {
            self.ws();
            items.push(self.value()?);
            self.ws();
            if self.eat(",") {
                continue;
            }
            if self.eat("]") {
                return Ok(Json::Array(items));
            }
            return Err(self.err("expected , or ]"));
        }
    }

    fn object(&mut self) -> Result<Json, String> {
        self.i += 1;
        let mut entries: Vec<(String, Json)> = Vec::new();
        self.ws();
        if self.eat("}") {
            return Ok(Json::Object(entries));
        }
        loop {
            self.ws();
            if self.s.get(self.i) != Some(&b'"') {
                return Err(self.err("expected a key in quotes"));
            }
            let key = self.string()?;
            self.ws();
            if !self.eat(":") {
                return Err(self.err("expected :"));
            }
            self.ws();
            let v = self.value()?;
            // a repeated key keeps its first position and its last value, as JSON.parse does
            if let Some(slot) = entries.iter_mut().find(|(k, _)| *k == key) {
                slot.1 = v;
            } else {
                entries.push((key, v));
            }
            self.ws();
            if self.eat(",") {
                continue;
            }
            if self.eat("}") {
                return Ok(Json::Object(entries));
            }
            return Err(self.err("expected , or }"));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_like_json_stringify() {
        let text = "{\n  \"a\": 1.50,\n  \"b\": [\n    true,\n    null,\n    \"x\\\"y\\\\z\\n\"\n  ],\n  \"c\": {},\n  \"d\": [],\n  \"e\": \"caf\u{e9} \u{1f600}\"\n}";
        let v = parse(text).unwrap();
        assert_eq!(v.pretty(), text);
    }

    #[test]
    fn escapes_and_errors() {
        assert_eq!(parse(r#""é😀\/""#).unwrap(), Json::String("\u{e9}\u{1f600}/".into()));
        assert!(parse("{\"a\": }").is_err());
        assert!(parse("[1, 2").is_err());
        assert!(parse("{} extra").is_err());
        assert!(parse("\u{feff}{}").is_ok());
    }

    #[test]
    fn edits_keep_order() {
        let mut v = parse(r#"{"z": 1, "a": 2}"#).unwrap();
        v.set("m", Json::Bool(true));
        v.set("z", Json::Null);
        assert!(v.remove("a"));
        assert!(!v.remove("a"));
        assert_eq!(v.pretty(), "{\n  \"z\": null,\n  \"m\": true\n}");
    }
}
