//! /bin/calc: standalone Desktop Calculator application built with `libui`.
#![no_std]
#![no_main]

use libui::client::{App, Args, Event};
use libui::gfx::rect;
use libui::state::{hit, Grid, TextField};
use libui::theme;

libui::app_main!(main, heap = 1 << 20);

const BTN_LABELS: [&str; 20] = [
    "C", "(", ")", "/",
    "7", "8", "9", "*",
    "4", "5", "6", "-",
    "1", "2", "3", "+",
    "0", "<-", "=", "AC",
];

struct Calculator {
    input: TextField<64>,
    display_buf: [u8; 64],
    display_len: usize,
}

impl Calculator {
    fn new() -> Self {
        Self {
            input: TextField::new(),
            display_buf: [0; 64],
            display_len: 0,
        }
    }

    fn push_char(&mut self, ch: char) {
        if self.input.len() < 40 {
            self.input.push(ch);
            self.update_display();
        }
    }

    fn backspace(&mut self) {
        self.input.backspace();
        self.update_display();
    }

    fn clear(&mut self) {
        self.input.clear();
        self.display_len = 0;
    }

    fn update_display(&mut self) {
        let s = self.input.as_str();
        self.display_len = s.len().min(64);
        self.display_buf[..self.display_len].copy_from_slice(s.as_bytes());
    }

    fn display(&self) -> &str {
        if self.display_len == 0 {
            "0"
        } else {
            core::str::from_utf8(&self.display_buf[..self.display_len]).unwrap_or("0")
        }
    }

    fn evaluate(&mut self) {
        let text = self.input.as_str();
        if let Some(val) = eval_expr(text) {
            let mut num_buf = [0u8; 32];
            let len = format_i64(val, &mut num_buf);
            self.input.clear();
            if let Ok(s) = core::str::from_utf8(&num_buf[..len]) {
                self.input.set_text(s);
            }
            self.update_display();
        }
    }

    fn handle_button(&mut self, label: &str) {
        match label {
            "C" => self.backspace(),
            "AC" => self.clear(),
            "=" => self.evaluate(),
            "<-" => self.backspace(),
            s => {
                for ch in s.chars() {
                    self.push_char(ch);
                }
            }
        }
    }

    fn handle_key(&mut self, key: u8) -> bool {
        match key {
            b'0'..=b'9' | b'+' | b'-' | b'*' | b'/' | b'(' | b')' | b'.' => {
                self.push_char(key as char);
                true
            }
            b'\r' | b'\n' | b'=' => {
                self.evaluate();
                true
            }
            0x08 | 127 => {
                self.backspace();
                true
            }
            27 | b'c' | b'C' => {
                self.clear();
                true
            }
            _ => false,
        }
    }
}

fn eval_expr(s: &str) -> Option<i64> {
    let bytes = s.as_bytes();
    let mut i = 0;

    fn parse_number(bytes: &[u8], i: &mut usize) -> Option<i64> {
        while *i < bytes.len() && bytes[*i] == b' ' {
            *i += 1;
        }
        let neg = if *i < bytes.len() && bytes[*i] == b'-' {
            *i += 1;
            true
        } else {
            false
        };
        let start = *i;
        let mut val = 0i64;
        while *i < bytes.len() && bytes[*i].is_ascii_digit() {
            let digit = (bytes[*i] - b'0') as i64;
            val = val.checked_mul(10)?.checked_add(digit)?;
            *i += 1;
        }
        if *i == start {
            return None;
        }
        Some(if neg { -val } else { val })
    }

    let mut left = parse_number(bytes, &mut i)?;
    while i < bytes.len() {
        while i < bytes.len() && bytes[i] == b' ' {
            i += 1;
        }
        if i >= bytes.len() {
            break;
        }
        let op = bytes[i];
        i += 1;
        let right = parse_number(bytes, &mut i)?;
        left = match op {
            b'+' => left.checked_add(right)?,
            b'-' => left.checked_sub(right)?,
            b'*' => left.checked_mul(right)?,
            b'/' => {
                if right == 0 {
                    return None;
                }
                left.checked_div(right)?
            }
            _ => return None,
        };
    }
    Some(left)
}

fn format_i64(mut n: i64, buf: &mut [u8; 32]) -> usize {
    if n == 0 {
        buf[0] = b'0';
        return 1;
    }
    let neg = n < 0;
    if neg {
        n = -n;
    }
    let mut temp = [0u8; 32];
    let mut i = 0;
    while n > 0 {
        temp[i] = b'0' + (n % 10) as u8;
        n /= 10;
        i += 1;
    }
    let mut out_len = 0;
    if neg {
        buf[out_len] = b'-';
        out_len += 1;
    }
    while i > 0 {
        i -= 1;
        buf[out_len] = temp[i];
        out_len += 1;
    }
    out_len
}

fn render(app: &mut App, calc: &Calculator, grid: &Grid) {
    let damage = {
        let mut p = app.painter().with_damage();
        p.fill(theme::PANEL);

        // Display box at top
        let display_rect = rect(12, 12, p.width.saturating_sub(24), 36);
        p.text_input(display_rect, calc.display(), false);

        // Grid buttons
        for (idx, &label) in BTN_LABELS.iter().enumerate() {
            let r = grid.cell(idx);
            if label == "=" {
                p.button_accent(r, label, false);
            } else {
                p.button(r, label, false, false);
            }
        }

        p.finish()
    };

    app.present(damage);
}

fn main(_args: Args) -> i32 {
    let Some(mut app) = App::connect(280, 340, "Calculator") else {
        return 1;
    };
    let mut calc = Calculator::new();
    let grid = Grid::new(12, 60, 4, 58, 48, 8);

    render(&mut app, &calc, &grid);

    loop {
        match app.next_event() {
            Event::Configure => {
                render(&mut app, &calc, &grid);
            }
            Event::Close => return 0,
            Event::Key(b) => {
                if calc.handle_key(b) {
                    render(&mut app, &calc, &grid);
                }
            }
            Event::Click(x, y) => {
                let mut clicked = false;
                for (idx, &label) in BTN_LABELS.iter().enumerate() {
                    let cell = grid.cell(idx);
                    if hit(cell, x, y) {
                        calc.handle_button(label);
                        clicked = true;
                        break;
                    }
                }
                if clicked {
                    render(&mut app, &calc, &grid);
                }
            }
            _ => {}
        }
    }
}
