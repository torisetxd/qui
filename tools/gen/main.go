// Command gen writes the wire constants, the Style and TextStyle structs with
// their encoders and class merges, both node decoders, and the generated parts
// of the docs, all from tools/gen/spec.txt. With -check it writes nothing and
// fails if any of them is stale.
package main

import (
	"bufio"
	"bytes"
	"flag"
	"fmt"
	"go/format"
	"log"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

type entry struct {
	name  string
	value int
}

type field struct {
	name, typ, def string
}

type setter struct {
	name, only string
}

type op struct {
	name   string
	code   int
	fields []field
	sets   []setter
	native string
	web    string
	doc    string
}

type styleField struct {
	strct, name, typ, op, goName, doc string
}

type spec struct {
	version int
	groups  map[string][]entry
	ops     []*op
	opByKey map[string]*op
	styles  []*styleField
	structs []string
	themes  []string
	values  map[string]map[string][4]float64
}

func parse(path string) spec {
	f, err := os.Open(path)
	if err != nil {
		log.Fatal(err)
	}
	defer f.Close()

	type logical struct {
		text string
		line int
	}
	var lines []logical
	scan := bufio.NewScanner(f)
	for n := 1; scan.Scan(); n++ {
		raw := scan.Text()
		text := strings.TrimSpace(raw)
		switch {
		case text == "" || strings.HasPrefix(text, "#"):
			lines = append(lines, logical{})
		case raw[0] == ' ' || raw[0] == '\t':
			if len(lines) == 0 || lines[len(lines)-1].text == "" {
				log.Fatalf("spec.txt:%d: continuation line without an entry", n)
			}
			lines[len(lines)-1].text += " " + text
		default:
			lines = append(lines, logical{text, n})
		}
	}

	s := spec{groups: map[string][]entry{}, values: map[string]map[string][4]float64{}, opByKey: map[string]*op{}}
	codes := map[int]string{}
	for _, l := range lines {
		if l.text == "" {
			continue
		}
		fail := func(format string, args ...any) {
			log.Fatalf("spec.txt:%d: %s", l.line, fmt.Sprintf(format, args...))
		}
		text, doc := l.text, ""
		if i := strings.IndexByte(text, '"'); i >= 0 {
			if !strings.HasSuffix(text, `"`) || len(text) == i+1 {
				fail("a doc string must end the entry")
			}
			text, doc = text[:i], text[i+1:len(text)-1]
		}
		fields := strings.Fields(text)
		switch fields[0] {
		case "version":
			s.version, _ = strconv.Atoi(fields[1])
		case "theme":
			if len(fields) != 7 {
				fail("theme needs a name, token and four channels")
			}
			if _, ok := s.values[fields[1]]; !ok {
				s.themes = append(s.themes, fields[1])
				s.values[fields[1]] = map[string][4]float64{}
			}
			var c [4]float64
			for i := range c {
				v, err := strconv.ParseFloat(fields[3+i], 64)
				if err != nil {
					fail("%v", err)
				}
				c[i] = v
			}
			s.values[fields[1]][fields[2]] = c
		case "op":
			if len(fields) < 3 {
				fail("expected `op Name code ...`")
			}
			code, err := strconv.Atoi(fields[2])
			if err != nil || code < 0 || code > 255 {
				fail("bad op code %q", fields[2])
			}
			if other, taken := codes[code]; taken {
				fail("op code %d is taken by %s", code, other)
			}
			if _, taken := s.opByKey[fields[1]]; taken {
				fail("op %s is defined twice", fields[1])
			}
			codes[code] = fields[1]
			o := &op{name: fields[1], code: code, doc: doc}
			for _, token := range fields[3:] {
				switch {
				case strings.HasPrefix(token, "native="):
					o.native = strings.TrimPrefix(token, "native=")
				case strings.HasPrefix(token, "web="):
					o.web = strings.TrimPrefix(token, "web=")
				case strings.HasPrefix(token, "+"):
					name, only, _ := strings.Cut(token[1:], "@")
					o.sets = append(o.sets, setter{name, only})
				case strings.Contains(token, ":"):
					name, typ, _ := strings.Cut(token, ":")
					typ, def, _ := strings.Cut(typ, "=")
					o.fields = append(o.fields, field{name, typ, def})
				default:
					fail("unexpected %q", token)
				}
			}
			for _, mode := range []string{o.native, o.web} {
				if mode != "" && mode != "custom" && mode != "skip" {
					fail("backend mode must be custom or skip, not %q", mode)
				}
			}
			s.ops = append(s.ops, o)
			s.opByKey[o.name] = o
		case "style":
			if len(fields) < 4 {
				fail("expected `style Struct field type [Op]`")
			}
			sf := &styleField{strct: fields[1], name: fields[2], typ: fields[3], doc: doc, goName: pascal(fields[2])}
			for _, token := range fields[4:] {
				if name, ok := strings.CutPrefix(token, "go="); ok {
					sf.goName = name
				} else {
					sf.op = token
				}
			}
			if _, ok := styleTypes[sf.typ]; !ok {
				fail("unknown style type %q", sf.typ)
			}
			if !contains(s.structs, sf.strct) {
				s.structs = append(s.structs, sf.strct)
			}
			s.styles = append(s.styles, sf)
		default:
			if len(fields) != 3 {
				fail("expected `group name value`")
			}
			v, err := strconv.Atoi(fields[2])
			if err != nil {
				fail("%v", err)
			}
			s.groups[fields[0]] = append(s.groups[fields[0]], entry{fields[1], v})
		}
	}
	for _, theme := range s.themes {
		for _, token := range s.groups["token"] {
			if _, ok := s.values[theme][token.name]; !ok {
				log.Fatalf("theme %s is missing token %s", theme, token.name)
			}
		}
	}
	sorted := append([]*op(nil), s.ops...)
	sort.SliceStable(sorted, func(i, j int) bool { return sorted[i].code < sorted[j].code })
	for _, o := range sorted {
		s.groups["op"] = append(s.groups["op"], entry{o.name, o.code})
	}
	for _, sf := range s.styles {
		needsOp := sf.typ != "class" && sf.typ != "key" && sf.typ != "part"
		if needsOp && s.opByKey[sf.op] == nil {
			log.Fatalf("spec.txt: %s.%s needs an op, and %q is not one", sf.strct, sf.name, sf.op)
		}
		if !needsOp && sf.op != "" {
			log.Fatalf("spec.txt: %s.%s is a %s and sends no op of its own", sf.strct, sf.name, sf.typ)
		}
	}
	return s
}

func contains(list []string, v string) bool {
	for _, item := range list {
		if item == v {
			return true
		}
	}
	return false
}

func pascal(snake string) string {
	var b strings.Builder
	for _, part := range strings.Split(snake, "_") {
		if part != "" {
			b.WriteString(strings.ToUpper(part[:1]) + part[1:])
		}
	}
	return b.String()
}

func camel(snake string) string {
	p := pascal(snake)
	return strings.ToLower(p[:1]) + p[1:]
}

func wrap(text string, width int, prefix string) string {
	var b strings.Builder
	line := ""
	for _, word := range strings.Fields(text) {
		if line != "" && len(prefix)+len(line)+1+len(word) > width {
			b.WriteString(prefix + line + "\n")
			line = ""
		}
		if line != "" {
			line += " "
		}
		line += word
	}
	if line != "" {
		b.WriteString(prefix + line + "\n")
	}
	return b.String()
}

func pack(c [4]float64) uint32 {
	b := func(v float64) uint32 { return uint32(math.Round(v * 255)) }
	return b(c[0])<<24 | b(c[1])<<16 | b(c[2])<<8 | b(c[3])
}

var cppEnums = []struct{ group, typeName, base, prefix string }{
	{"op", "Op", "uint8_t", "Op"},
	{"kind", "Kind", "uint8_t", "Kind"},
	{"dir", "Dir", "uint8_t", "Dir"},
	{"size", "SizeMode", "uint8_t", "Size"},
	{"fit", "Fit", "uint8_t", "Fit"},
	{"span", "SpanFlag", "uint8_t", "Span"},
	{"flag", "Flag", "uint32_t", "Flag"},
	{"token", "Token", "uint8_t", "Token"},
}

const banner = "Generated by tools/gen from tools/gen/spec.txt. Do not edit."

func cpp(s spec) string {
	var b strings.Builder
	b.WriteString("// " + banner + "\n#pragma once\n\n#include <cstdint>\n\nnamespace qui {\n\nnamespace wire {\n\n")
	fmt.Fprintf(&b, "inline constexpr uint8_t kVersion = %d;\n", s.version)
	fmt.Fprintf(&b, "inline constexpr int kThemeCount = %d;\n", len(s.themes))
	fmt.Fprintf(&b, "inline constexpr int kTokenCount = %d;\n\n", len(s.groups["token"]))
	for _, e := range cppEnums {
		fmt.Fprintf(&b, "enum %s : %s {\n", e.typeName, e.base)
		for _, v := range s.groups[e.group] {
			fmt.Fprintf(&b, "  %s%s = %d,\n", e.prefix, v.name, v.value)
		}
		b.WriteString("};\n\n")
	}
	fmt.Fprintf(&b, "inline constexpr uint32_t kThemes[kThemeCount][kTokenCount + 1] = {\n")
	for _, theme := range s.themes {
		b.WriteString("    {0")
		for _, token := range s.groups["token"] {
			fmt.Fprintf(&b, ", 0x%08x", pack(s.values[theme][token.name]))
		}
		b.WriteString("},\n")
	}
	b.WriteString("};\n\n}  // namespace wire\n\n")
	for _, v := range s.groups["align"] {
		fmt.Fprintf(&b, "inline constexpr uint8_t %s = %d;\n", v.name, v.value)
	}
	b.WriteString("\n")
	for _, v := range s.groups["role"] {
		fmt.Fprintf(&b, "inline constexpr uint8_t Role%s = %d;\n", v.name, v.value)
	}
	b.WriteString("\n")
	for i, theme := range s.themes {
		fmt.Fprintf(&b, "inline constexpr uint8_t %s = %d;\n", theme, i)
	}
	b.WriteString("\n")
	for _, v := range s.groups["token"] {
		fmt.Fprintf(&b, "inline constexpr uint32_t %s = %du << 24;\n", v.name, v.value)
	}
	b.WriteString("\n")
	for _, v := range s.groups["key"] {
		fmt.Fprintf(&b, "inline constexpr uint32_t Key%s = %d;\n", v.name, v.value)
	}
	b.WriteString("\n")
	for _, v := range s.groups["mod"] {
		fmt.Fprintf(&b, "inline constexpr uint32_t Mod%s = %d;\n", v.name, v.value)
	}
	b.WriteString("\n")
	for _, v := range s.groups["gesture"] {
		fmt.Fprintf(&b, "inline constexpr uint8_t Gesture%s = %d;\n", v.name, v.value)
	}
	b.WriteString("\n}  // namespace qui\n")
	return b.String()
}

func goSource(s spec) string {
	var b strings.Builder
	b.WriteString("// " + banner + "\n\npackage qui\n\n")
	fmt.Fprintf(&b, "const wireVersion = %d\n\n", s.version)
	for _, group := range []string{"op", "kind", "dir", "size", "flag"} {
		b.WriteString("const (\n")
		for _, v := range s.groups[group] {
			fmt.Fprintf(&b, "\t%s%s = %d\n", group, v.name, v.value)
		}
		b.WriteString(")\n\n")
	}
	b.WriteString("const (\n")
	for _, v := range s.groups["fit"] {
		fmt.Fprintf(&b, "\tFit%s ImageFit = %d\n", v.name, v.value)
	}
	b.WriteString(")\n\nconst (\n")
	for _, v := range s.groups["span"] {
		fmt.Fprintf(&b, "\tSpan%s SpanFlags = %d\n", v.name, v.value)
	}
	b.WriteString(")\n\nconst (\n")
	for _, v := range s.groups["align"] {
		fmt.Fprintf(&b, "\t%s Align = %d\n", v.name, v.value)
	}
	b.WriteString(")\n\nconst (\n")
	for _, v := range s.groups["role"] {
		fmt.Fprintf(&b, "\tRole%s Role = %d\n", v.name, v.value)
	}
	b.WriteString(")\n\nconst (\n")
	for i, theme := range s.themes {
		fmt.Fprintf(&b, "\t%s Theme = %d\n", theme, i)
	}
	b.WriteString(")\n\nconst (\n")
	for _, v := range s.groups["token"] {
		fmt.Fprintf(&b, "\t%s Color = %d << 24\n", v.name, v.value)
	}
	b.WriteString(")\n\nconst (\n")
	for _, v := range s.groups["key"] {
		fmt.Fprintf(&b, "\tKey%s Key = %d\n", v.name, v.value)
	}
	b.WriteString(")\n\nconst (\n")
	for _, v := range s.groups["mod"] {
		fmt.Fprintf(&b, "\tMod%s Mods = %d\n", v.name, v.value)
	}
	b.WriteString(")\n\nconst (\n")
	for _, v := range s.groups["gesture"] {
		fmt.Fprintf(&b, "\tGesture%s Gestures = %d\n", v.name, v.value)
	}
	b.WriteString(")\n")
	return b.String()
}

var jsRead = map[string]string{
	"u8": "r.u8()", "u16": "r.u16()", "u32": "r.u32()", "i32": "(r.u32() | 0)", "f32": "r.f32()",
	"bool": "r.u8() !== 0", "str": "r.str()", "f32x4": "[r.f32(), r.f32(), r.f32(), r.f32()]",
}

func js(s spec) string {
	var b strings.Builder
	b.WriteString("// " + banner + "\n\n")
	fmt.Fprintf(&b, "export const Version = %d;\n\n", s.version)
	for _, e := range []struct{ group, name string }{
		{"op", "Op"}, {"kind", "Kind"}, {"dir", "Dir"}, {"size", "Size"},
		{"fit", "Fit"}, {"span", "Span"}, {"align", "Align"}, {"role", "Role"}, {"flag", "Flag"}, {"key", "Key"}, {"mod", "Mod"}, {"gesture", "Gesture"},
	} {
		fmt.Fprintf(&b, "export const %s = {", e.name)
		for i, v := range s.groups[e.group] {
			if i > 0 {
				b.WriteString(", ")
			}
			fmt.Fprintf(&b, "%s: %d", v.name, v.value)
		}
		b.WriteString("};\n")
	}
	b.WriteString("\nexport const Tokens = [\"\"")
	for _, v := range s.groups["token"] {
		fmt.Fprintf(&b, ", %q", v.name)
	}
	b.WriteString("];\n\nexport const Themes = [\n")
	for _, theme := range s.themes {
		b.WriteString("  {")
		for i, token := range s.groups["token"] {
			if i > 0 {
				b.WriteString(", ")
			}
			c := s.values[theme][token.name]
			fmt.Fprintf(&b, "%s: \"rgba(%d,%d,%d,%.3g)\"", token.name,
				int(math.Round(c[0]*255)), int(math.Round(c[1]*255)), int(math.Round(c[2]*255)), c[3])
		}
		b.WriteString("},\n")
	}
	b.WriteString("];\n\n")

	b.WriteString("// Reads the payload of a node property op into `node` and returns true, or\n// returns false for ops that qui.js decodes itself.\nexport function decodeProps(op, node, r) {\n  switch (op) {\n")
	for _, o := range s.ops {
		if o.web == "custom" {
			continue
		}
		fmt.Fprintf(&b, "    case Op.%s:", o.name)
		for _, f := range o.fields {
			read, ok := jsRead[f.typ]
			if f.typ == "u64" {
				read, ok = "(r.u32(), r.u32())", o.web == "skip"
			}
			if !ok {
				log.Fatalf("op %s: the web decoder cannot read %s; mark it web=custom", o.name, f.typ)
			}
			if o.web == "skip" {
				fmt.Fprintf(&b, " %s;", read)
			} else {
				fmt.Fprintf(&b, " node.%s = %s;", camel(f.name), read)
			}
		}
		if o.web != "skip" {
			for _, set := range o.sets {
				if set.only == "" || set.only == "web" {
					fmt.Fprintf(&b, " node.%s = true;", camel(set.name))
				}
			}
		}
		b.WriteString(" return true;\n")
	}
	b.WriteString("    default: return false;\n  }\n}\n")
	return b.String()
}

var cppRead = map[string]string{
	"u8": "uint8_t", "u16": "uint16_t", "u32": "uint32_t", "i32": "int32_t", "u64": "uint64_t", "f32": "float", "bool": "uint8_t",
}

var cppStorage = map[string]string{
	"u8": "uint8_t", "u16": "uint16_t", "u32": "uint32_t", "i32": "int32_t", "u64": "uint64_t", "f32": "float", "bool": "bool",
	"str": "std::string_view", "f32x4": "float",
}

var storageRank = map[string]int{"str": 0, "u64": 1, "f32": 2, "u32": 2, "i32": 2, "f32x4": 3, "u16": 4, "u8": 5, "bool": 6}

func nodeCpp(s spec) string {
	type prop struct {
		name, typ, def string
	}
	var props []prop
	seen := map[string]string{}
	add := func(name, typ, def, from string) {
		if t, ok := seen[name]; ok {
			if t != typ {
				log.Fatalf("node field %s is %s in one op and %s in %s", name, t, typ, from)
			}
			return
		}
		seen[name] = typ
		props = append(props, prop{name, typ, def})
	}
	for _, o := range s.ops {
		if o.native != "" {
			continue
		}
		for _, f := range o.fields {
			if _, ok := cppStorage[f.typ]; !ok {
				log.Fatalf("op %s: the engine decoder cannot read %s; mark it native=custom", o.name, f.typ)
			}
			add(f.name, f.typ, f.def, o.name)
		}
		for _, set := range o.sets {
			if set.only == "" || set.only == "native" {
				add(set.name, "bool", "", o.name)
			}
		}
	}
	sort.SliceStable(props, func(i, j int) bool { return storageRank[props[i].typ] < storageRank[props[j].typ] })

	var b strings.Builder
	b.WriteString("// " + banner + "\n#pragma once\n\n#include \"wire.gen.hpp\"\n\n#include <cstdint>\n#include <string_view>\n\nnamespace qui {\n\n")
	b.WriteString("// The node properties that arrive as plain op payloads. Ops marked\n// native=custom in spec.txt are decoded by hand in engine.cpp instead.\nstruct NodeProps {\n")
	for _, p := range props {
		switch {
		case p.typ == "f32x4":
			fmt.Fprintf(&b, "  float %s[4] = {};\n", p.name)
		case p.typ == "str":
			fmt.Fprintf(&b, "  std::string_view %s;\n", p.name)
		case p.typ == "bool":
			fmt.Fprintf(&b, "  bool %s = false;\n", p.name)
		default:
			def := p.def
			if def == "" {
				def = "0"
			}
			fmt.Fprintf(&b, "  %s %s = %s;\n", cppStorage[p.typ], p.name, def)
		}
	}
	b.WriteString("};\n\n")
	b.WriteString("// Reads the payload of a node property op into `n` and returns true, or\n// returns false for ops the engine decodes itself.\n")
	b.WriteString("template <class Reader>\nbool decode_props(uint8_t op, NodeProps &n, Reader &in) {\n  switch (op) {\n")
	for _, o := range s.ops {
		if o.native == "custom" {
			continue
		}
		fmt.Fprintf(&b, "    case wire::Op%s:", o.name)
		for _, f := range o.fields {
			var read string
			switch f.typ {
			case "str":
				read = "in.str()"
			case "f32x4":
				if o.native == "skip" {
					read = "(in.template read<float>(), in.template read<float>(), in.template read<float>(), in.template read<float>())"
				} else {
					fmt.Fprintf(&b, " for (float &v : n.%s) v = in.template read<float>();", f.name)
					continue
				}
			case "bool":
				read = "in.template read<uint8_t>() != 0"
			default:
				t, ok := cppRead[f.typ]
				if !ok {
					log.Fatalf("op %s: the engine decoder cannot read %s; mark it native=custom", o.name, f.typ)
				}
				read = "in.template read<" + t + ">()"
			}
			if o.native == "skip" {
				fmt.Fprintf(&b, " static_cast<void>(%s);", read)
			} else {
				fmt.Fprintf(&b, " n.%s = %s;", f.name, read)
			}
		}
		if o.native != "skip" {
			for _, set := range o.sets {
				if set.only == "" || set.only == "native" {
					fmt.Fprintf(&b, " n.%s = true;", set.name)
				}
			}
		}
		b.WriteString(" return true;\n")
	}
	b.WriteString("    default: return false;\n  }\n}\n\n}  // namespace qui\n")
	return b.String()
}

var styleTypes = map[string]struct{ cpp, cppInit, goType string }{
	"class":    {"std::string_view", "{}", "string"},
	"key":      {"std::string_view", "{}", "string"},
	"string":   {"std::string_view", "{}", "string"},
	"size":     {"Size", "{}", "Size"},
	"f32":      {"float", " = 0", "float32"},
	"part":     {"float", " = 0", "float32"},
	"motion":   {"float", " = 0", "float32"},
	"edges":    {"Edges", "{}", "Edges"},
	"align":    {"uint8_t", " = 0", "Align"},
	"gestures": {"uint8_t", " = 0", "Gestures"},
	"role":     {"uint8_t", " = 0", "Role"},
	"pos":      {"bool", " = false", "bool"},
	"flag":     {"bool", " = false", "bool"},
	"on":       {"bool", " = false", "bool"},
	"i32":      {"int32_t", " = 0", "int32"},
	"color":    {"Color", " = 0", "Color"},
	"border":   {"Color", " = 0", "Color"},
	"gradient": {"Gradient", "{}", "Gradient"},
	"shadow":   {"BoxShadow", "{}", "BoxShadow"},
	"optf32":   {"std::optional<float>", "{}", "*float32"},
}

func (s spec) fields(strct string) []*styleField {
	var out []*styleField
	for _, sf := range s.styles {
		if sf.strct == strct {
			out = append(out, sf)
		}
	}
	return out
}

func has(fields []*styleField, name string) bool {
	for _, sf := range fields {
		if sf.name == name {
			return true
		}
	}
	return false
}

func snake(camelName string) string {
	var b strings.Builder
	for i, r := range camelName {
		if r >= 'A' && r <= 'Z' {
			if i > 0 {
				b.WriteByte('_')
			}
			r += 'a' - 'A'
		}
		b.WriteRune(r)
	}
	return b.String()
}

func cppStructs(s spec) string {
	var b strings.Builder
	for _, strct := range s.structs {
		fmt.Fprintf(&b, "struct %s {\n", strct)
		for _, sf := range s.fields(strct) {
			b.WriteString(wrap(sf.doc, 80, "  // "))
			t := styleTypes[sf.typ]
			fmt.Fprintf(&b, "  %s %s%s;\n", t.cpp, sf.name, t.cppInit)
		}
		b.WriteString("};\n\n")
	}
	return b.String()
}

func cppStyle(s spec) string {
	var b strings.Builder
	b.WriteString("// " + banner + "\n// qui.hpp includes it inside namespace qui, after Size, Edges, Gradient and\n// BoxShadow.\n\n")
	b.WriteString(cppStructs(s))
	b.WriteString(`namespace gen {

inline void raw(std::vector<uint8_t> &b, const void *data, size_t len) {
  const size_t at = b.size();
  b.resize(at + len);
  std::memcpy(b.data() + at, data, len);
}
inline void u8(std::vector<uint8_t> &b, uint8_t op, uint8_t v) {
  b.push_back(op);
  b.push_back(v);
}
inline void u32(std::vector<uint8_t> &b, uint8_t op, uint32_t v) {
  b.push_back(op);
  raw(b, &v, 4);
}
inline void f32(std::vector<uint8_t> &b, uint8_t op, float v) {
  b.push_back(op);
  raw(b, &v, 4);
}
inline void f32s(std::vector<uint8_t> &b, uint8_t op, std::initializer_list<float> values) {
  b.push_back(op);
  for (const float v : values) raw(b, &v, 4);
}
inline void str(std::vector<uint8_t> &b, uint8_t op, std::string_view v) {
  u32(b, op, static_cast<uint32_t>(v.size()));
  raw(b, v.data(), v.size());
}
inline void size(std::vector<uint8_t> &b, uint8_t op, Size s) {
  if (s.mode == wire::SizeFit) return;
  u8(b, op, s.mode);
  raw(b, &s.value, 4);
}
inline void gradient(std::vector<uint8_t> &b, uint8_t op, const Gradient &g) {
  u32(b, op, g.from);
  raw(b, &g.to, 4);
  raw(b, &g.angle, 4);
}
inline void shadow(std::vector<uint8_t> &b, uint8_t op, const BoxShadow &s) {
  u32(b, op, s.color);
  raw(b, &s.blur, 4);
  raw(b, &s.y, 4);
}
inline void color_width(std::vector<uint8_t> &b, uint8_t op, Color color, float width) {
  u32(b, op, color);
  raw(b, &width, 4);
}

`)
	for _, strct := range s.structs {
		fs := s.fields(strct)
		fmt.Fprintf(&b, "inline void encode(std::vector<uint8_t> &b, const %s &s, [[maybe_unused]] bool reduce_motion) {\n", strct)
		for _, sf := range fs {
			v := "s." + sf.name
			o := "wire::Op" + sf.op
			switch sf.typ {
			case "size":
				fmt.Fprintf(&b, "  size(b, %s, %s);\n", o, v)
			case "f32":
				fmt.Fprintf(&b, "  if (%s != 0) f32(b, %s, %s);\n", v, o, v)
			case "motion":
				fmt.Fprintf(&b, "  if (%s != 0 && !reduce_motion) f32(b, %s, %s);\n", v, o, v)
			case "align", "gestures", "role":
				fmt.Fprintf(&b, "  if (%s) u8(b, %s, %s);\n", v, o, v)
			case "i32":
				fmt.Fprintf(&b, "  if (%s) u32(b, %s, static_cast<uint32_t>(%s));\n", v, o, v)
			case "color":
				fmt.Fprintf(&b, "  if (%s) u32(b, %s, %s);\n", v, o, v)
			case "flag":
				fmt.Fprintf(&b, "  if (%s) b.push_back(%s);\n", v, o)
			case "on":
				fmt.Fprintf(&b, "  if (%s) u8(b, %s, 1);\n", v, o)
			case "string":
				fmt.Fprintf(&b, "  if (!%s.empty()) str(b, %s, %s);\n", v, o, v)
			case "optf32":
				fmt.Fprintf(&b, "  if (%s) f32(b, %s, *%s);\n", v, o, v)
			case "edges":
				fmt.Fprintf(&b, "  if (%[1]s.top != 0 || %[1]s.right != 0 || %[1]s.bottom != 0 || %[1]s.left != 0) f32s(b, %[2]s, {%[1]s.top, %[1]s.right, %[1]s.bottom, %[1]s.left});\n", v, o)
			case "gradient":
				fmt.Fprintf(&b, "  if (%[1]s.from || %[1]s.to) gradient(b, %[2]s, %[1]s);\n", v, o)
			case "shadow":
				fmt.Fprintf(&b, "  if (%[1]s.color) shadow(b, %[2]s, %[1]s);\n", v, o)
			case "pos":
				cond := v
				if has(fs, "fixed") {
					cond += " || s.fixed"
				}
				fmt.Fprintf(&b, "  if (%s) f32s(b, %s, {s.x, s.y});\n", cond, o)
			case "border":
				fmt.Fprintf(&b, "  if (%[1]s) color_width(b, %[2]s, %[1]s, %[1]s_w != 0 ? %[1]s_w : 1);\n", v, o)
			}
		}
		b.WriteString("}\n\n")

		fmt.Fprintf(&b, "// Fills the fields of `s` left at zero from the class `b`.\ninline void merge(%s &s, const %s &b) {\n", strct, strct)
		for _, sf := range fs {
			v, w := "s."+sf.name, "b."+sf.name
			switch sf.typ {
			case "size":
				fmt.Fprintf(&b, "  if (%s.mode == wire::SizeFit) %s = %s;\n", v, v, w)
			case "f32", "motion":
				fmt.Fprintf(&b, "  if (%s == 0) %s = %s;\n", v, v, w)
			case "align", "gestures", "role", "i32", "color":
				fmt.Fprintf(&b, "  if (!%s) %s = %s;\n", v, v, w)
			case "string":
				fmt.Fprintf(&b, "  if (%s.empty()) %s = %s;\n", v, v, w)
			case "flag", "on":
				fmt.Fprintf(&b, "  %s = %s || %s;\n", v, v, w)
			case "edges":
				fmt.Fprintf(&b, "  if (%[1]s.top == 0 && %[1]s.right == 0 && %[1]s.bottom == 0 && %[1]s.left == 0) %[1]s = %[2]s;\n", v, w)
			case "gradient":
				fmt.Fprintf(&b, "  if (!%[1]s.from && !%[1]s.to) %[1]s = %[2]s;\n", v, w)
			case "shadow":
				fmt.Fprintf(&b, "  if (!%[1]s.color) %[1]s = %[2]s;\n", v, w)
			case "pos":
				fmt.Fprintf(&b, "  if (!%[1]s) {\n    %[1]s = %[2]s;\n    s.x = b.x;\n    s.y = b.y;\n  }\n", v, w)
			case "border":
				fmt.Fprintf(&b, "  if (!%[1]s) {\n    %[1]s = %[2]s;\n    %[1]s_w = %[2]s_w;\n  }\n", v, w)
			}
		}
		b.WriteString("}\n\n")
	}
	b.WriteString("}  // namespace gen\n")
	return b.String()
}

func goStyle(s spec) string {
	var b strings.Builder
	b.WriteString("// " + banner + "\n\npackage qui\n\n")
	for _, strct := range s.structs {
		fs := s.fields(strct)
		fmt.Fprintf(&b, "type %s struct {\n", strct)
		for _, sf := range fs {
			b.WriteString(wrap(sf.doc, 80, "\t// "))
			fmt.Fprintf(&b, "\t%s %s\n", sf.goName, styleTypes[sf.typ].goType)
		}
		b.WriteString("}\n\n")
	}
	for _, strct := range s.structs {
		fs := s.fields(strct)
		goName := func(name string) string {
			for _, sf := range fs {
				if sf.name == name {
					return sf.goName
				}
			}
			log.Fatalf("%s has no field %s", strct, name)
			return ""
		}
		fmt.Fprintf(&b, "func (u *Ui) encode%s(s *%s) {\n", strct, strct)
		for _, sf := range fs {
			v := "s." + sf.goName
			o := "op" + sf.op
			switch sf.typ {
			case "size":
				fmt.Fprintf(&b, "\tu.sizeOp(%s, %s)\n", o, v)
			case "f32":
				fmt.Fprintf(&b, "\tif %s != 0 {\n\t\tu.f32Op(%s, %s)\n\t}\n", v, o, v)
			case "motion":
				fmt.Fprintf(&b, "\tif %s != 0 && !u.reduceMotion {\n\t\tu.f32Op(%s, %s)\n\t}\n", v, o, v)
			case "align", "gestures", "role":
				fmt.Fprintf(&b, "\tif %s != 0 {\n\t\tu.u8Op(%s, uint8(%s))\n\t}\n", v, o, v)
			case "i32", "color":
				fmt.Fprintf(&b, "\tif %s != 0 {\n\t\tu.u32Op(%s, uint32(%s))\n\t}\n", v, o, v)
			case "flag":
				fmt.Fprintf(&b, "\tif %s {\n\t\tu.u8(%s)\n\t}\n", v, o)
			case "on":
				fmt.Fprintf(&b, "\tif %s {\n\t\tu.u8Op(%s, 1)\n\t}\n", v, o)
			case "string":
				fmt.Fprintf(&b, "\tif %s != \"\" {\n\t\tu.str(%s, %s)\n\t}\n", v, o, v)
			case "optf32":
				fmt.Fprintf(&b, "\tif %s != nil {\n\t\tu.f32Op(%s, *%s)\n\t}\n", v, o, v)
			case "edges":
				fmt.Fprintf(&b, "\tif %[1]s != (Edges{}) {\n\t\tu.u8(%[2]s)\n\t\tu.f32(%[1]s.Top)\n\t\tu.f32(%[1]s.Right)\n\t\tu.f32(%[1]s.Bottom)\n\t\tu.f32(%[1]s.Left)\n\t}\n", v, o)
			case "gradient":
				fmt.Fprintf(&b, "\tif %[1]s.From != 0 || %[1]s.To != 0 {\n\t\tu.u8(%[2]s)\n\t\tu.u32(uint32(%[1]s.From))\n\t\tu.u32(uint32(%[1]s.To))\n\t\tu.f32(%[1]s.Angle)\n\t}\n", v, o)
			case "shadow":
				fmt.Fprintf(&b, "\tif %[1]s.Color != 0 {\n\t\tu.u8(%[2]s)\n\t\tu.u32(uint32(%[1]s.Color))\n\t\tu.f32(%[1]s.Blur)\n\t\tu.f32(%[1]s.Y)\n\t}\n", v, o)
			case "pos":
				cond := v
				if has(fs, "fixed") {
					cond += " || s." + goName("fixed")
				}
				fmt.Fprintf(&b, "\tif %s {\n\t\tu.u8(%s)\n\t\tu.f32(s.%s)\n\t\tu.f32(s.%s)\n\t}\n", cond, o, goName("x"), goName("y"))
			case "border":
				width := "s." + goName(sf.name+"_w")
				fmt.Fprintf(&b, "\tif %[1]s != 0 {\n\t\twidth := %[3]s\n\t\tif width == 0 {\n\t\t\twidth = 1\n\t\t}\n\t\tu.u8(%[2]s)\n\t\tu.u32(uint32(%[1]s))\n\t\tu.f32(width)\n\t}\n", v, o, width)
			}
		}
		b.WriteString("}\n\n")

		fmt.Fprintf(&b, "// fill returns s with the fields left at zero taken from the class b.\nfunc (s %s) fill(b %s) %s {\n", strct, strct, strct)
		for _, sf := range fs {
			v, w := "s."+sf.goName, "b."+sf.goName
			switch sf.typ {
			case "size":
				fmt.Fprintf(&b, "\tif %s.mode == sizeFit {\n\t\t%s = %s\n\t}\n", v, v, w)
			case "f32", "motion", "align", "gestures", "role", "i32", "color":
				fmt.Fprintf(&b, "\tif %s == 0 {\n\t\t%s = %s\n\t}\n", v, v, w)
			case "string":
				fmt.Fprintf(&b, "\tif %s == \"\" {\n\t\t%s = %s\n\t}\n", v, v, w)
			case "flag", "on":
				fmt.Fprintf(&b, "\t%s = %s || %s\n", v, v, w)
			case "edges":
				fmt.Fprintf(&b, "\tif %s == (Edges{}) {\n\t\t%s = %s\n\t}\n", v, v, w)
			case "gradient":
				fmt.Fprintf(&b, "\tif %[1]s.From == 0 && %[1]s.To == 0 {\n\t\t%[1]s = %[2]s\n\t}\n", v, w)
			case "shadow":
				fmt.Fprintf(&b, "\tif %[1]s.Color == 0 {\n\t\t%[1]s = %[2]s\n\t}\n", v, w)
			case "pos":
				fmt.Fprintf(&b, "\tif !%[1]s {\n\t\t%[1]s, s.%[3]s, s.%[4]s = %[2]s, b.%[3]s, b.%[4]s\n\t}\n", v, w, goName("x"), goName("y"))
			case "border":
				width := goName(sf.name + "_w")
				fmt.Fprintf(&b, "\tif %[1]s == 0 {\n\t\t%[1]s, s.%[3]s = %[2]s, b.%[3]s\n\t}\n", v, w, width)
			}
		}
		b.WriteString("\treturn s\n}\n\n")
	}
	return b.String()
}

func backendNote(o *op) string {
	switch {
	case o.web == "skip":
		return " Native only; the browser ignores it."
	case o.native == "skip":
		return " The engine ignores it."
	}
	return ""
}

func wireDoc(s spec) string {
	var b strings.Builder
	b.WriteString("| Op | Payload | Meaning |\n| --- | --- | --- |\n")
	for _, o := range s.ops {
		var payload []string
		for _, f := range o.fields {
			payload = append(payload, "`"+f.name+":"+f.typ+"`")
		}
		if len(payload) == 0 {
			payload = append(payload, "none")
		}
		doc := strings.ReplaceAll(o.doc+backendNote(o), "|", "\\|")
		fmt.Fprintf(&b, "| `%s` %d | %s | %s |\n", o.name, o.code, strings.Join(payload, " "), strings.TrimSpace(doc))
	}
	return b.String()
}

func skillDoc(s spec) string {
	var b strings.Builder
	for i, strct := range s.structs {
		var names []string
		for _, sf := range s.fields(strct) {
			names = append(names, sf.goName)
		}
		if i == 0 {
			fmt.Fprintf(&b, "`%s` fields, in declaration order (C++ names are the snake_case of these, and `Class` is `klass`): `%s`\n", strct, strings.Join(names, " "))
		} else {
			fmt.Fprintf(&b, "\n`%s` fields, in order: `%s`\n", strct, strings.Join(names, " "))
		}
	}
	return b.String()
}

// stripBodies replaces every function body in a C++ header with `;` and
// drops constructor initializer lists, leaving declarations and comments.
func stripBodies(src string) string {
	var out strings.Builder
	boundary := 0
	skipLiteral := func(i int) int {
		quote := src[i]
		for i++; i < len(src) && src[i] != quote; i++ {
			if src[i] == '\\' {
				i++
			}
		}
		return i
	}
	skipComment := func(i int) int {
		if strings.HasPrefix(src[i:], "//") {
			end := strings.IndexByte(src[i:], '\n')
			if end < 0 {
				return len(src) - 1
			}
			return i + end - 1
		}
		end := strings.Index(src[i+2:], "*/")
		return i + 2 + end + 1
	}
	matching := func(i int) int {
		depth := 0
		for ; i < len(src); i++ {
			switch {
			case src[i] == '"' || src[i] == '\'':
				i = skipLiteral(i)
			case strings.HasPrefix(src[i:], "//") || strings.HasPrefix(src[i:], "/*"):
				i = skipComment(i)
			case src[i] == '{':
				depth++
			case src[i] == '}':
				depth--
				if depth == 0 {
					return i
				}
			}
		}
		return len(src) - 1
	}
	parens := 0
	for i := 0; i < len(src); i++ {
		c := src[i]
		switch {
		case c == '"' || c == '\'':
			end := skipLiteral(i)
			out.WriteString(src[i : end+1])
			i = end
			continue
		case strings.HasPrefix(src[i:], "//") || strings.HasPrefix(src[i:], "/*"):
			end := skipComment(i)
			out.WriteString(src[i : end+1])
			i = end
			continue
		case c == '(' || c == ')':
			if c == '(' {
				parens++
			} else {
				parens--
			}
			out.WriteByte(c)
			continue
		case c == '{' && parens > 0:
			// A braced default argument, such as `Style style = {}`.
			end := matching(i)
			out.WriteString(src[i : end+1])
			i = end
			continue
		case c == ';' || c == '}':
			out.WriteByte(c)
			boundary = out.Len()
			continue
		case c != '{':
			out.WriteByte(c)
			continue
		}
		decl := out.String()[boundary:]
		code := stripComments(decl)
		trimmed := strings.TrimSpace(code)
		isBody := strings.Contains(trimmed, "(") && !strings.HasPrefix(trimmed, "struct ") && !strings.HasPrefix(trimmed, "class ") && !strings.HasPrefix(trimmed, "enum ") && !strings.HasPrefix(trimmed, "namespace ")
		if isBody {
			last := trimmed[len(trimmed)-1]
			isBody = last == ')' || strings.HasSuffix(trimmed, " const") || strings.HasSuffix(trimmed, " noexcept") || strings.HasSuffix(trimmed, " override")
		}
		if !isBody {
			out.WriteByte('{')
			boundary = out.Len()
			continue
		}
		if cut := initList(decl); cut >= 0 {
			text := out.String()[:boundary+cut]
			out.Reset()
			out.WriteString(text)
		} else {
			text := strings.TrimRight(out.String(), " ")
			out.Reset()
			out.WriteString(text)
		}
		out.WriteByte(';')
		boundary = out.Len()
		i = matching(i)
	}
	return out.String()
}

func stripComments(s string) string {
	var b strings.Builder
	for _, line := range strings.Split(s, "\n") {
		if i := strings.Index(line, "//"); i >= 0 {
			line = line[:i]
		}
		b.WriteString(line + "\n")
	}
	return b.String()
}

// initList returns where a constructor's initializer list starts in decl, the
// ` : ` after its parameters, or -1.
func initList(decl string) int {
	depth := 0
	for i := 0; i < len(decl); i++ {
		if strings.HasPrefix(decl[i:], "//") {
			end := strings.IndexByte(decl[i:], '\n')
			if end < 0 {
				return -1
			}
			i += end
			continue
		}
		switch decl[i] {
		case '(':
			depth++
		case ')':
			depth--
			if depth == 0 {
				rest := strings.TrimLeft(decl[i+1:], " \n")
				if strings.HasPrefix(rest, ":") && !strings.HasPrefix(rest, "::") {
					return i + 1
				}
				return -1
			}
		}
	}
	return -1
}

func cppDoc(s spec, header string) string {
	stripped := stripBodies(header)
	var b strings.Builder
	b.WriteString("# C++ API\n\n<!-- Generated by tools/gen from src/qui.hpp and tools/gen/spec.txt. Do not edit. -->\n\n")
	b.WriteString("Every public declaration of `src/qui.hpp`, with the bodies left out. Designated initializers must list fields in the order shown here. `Style` and `TextStyle` come from `tools/gen/spec.txt`.\n\n```cpp\n")
	private, blank := false, false
	for _, line := range strings.Split(stripped, "\n") {
		trimmed := strings.TrimSpace(line)
		switch {
		case strings.HasPrefix(trimmed, "#include \"style.gen.inc\""):
			b.WriteString(strings.TrimRight(cppStructs(s), "\n") + "\n")
			blank = false
			continue
		case strings.HasPrefix(trimmed, "#"), trimmed == "namespace qui {", strings.HasPrefix(trimmed, "}  // namespace"), trimmed == "public:":
			continue
		case trimmed == "private:":
			private = true
			continue
		case private && line == "};":
			private = false
		case private:
			continue
		}
		if trimmed == "" {
			if !blank {
				b.WriteString("\n")
			}
			blank = true
			continue
		}
		blank = false
		b.WriteString(line + "\n")
	}
	return strings.TrimRight(b.String(), "\n") + "\n```\n"
}

func replaceRegion(content, name, generated string) string {
	start, end := "<!-- gen:"+name+" -->\n", "<!-- /gen:"+name+" -->"
	i := strings.Index(content, start)
	j := strings.Index(content, end)
	if i < 0 || j < i {
		log.Fatalf("missing %q ... %q markers", start, end)
	}
	return content[:i+len(start)] + generated + content[j:]
}

func main() {
	check := flag.Bool("check", false, "fail if a generated file is stale instead of writing it")
	flag.Parse()
	root := "."
	if flag.NArg() > 0 {
		root = flag.Arg(0)
	}
	read := func(name string) string {
		data, err := os.ReadFile(filepath.Join(root, name))
		if err != nil {
			log.Fatal(err)
		}
		return string(data)
	}
	s := parse(filepath.Join(root, "tools/gen/spec.txt"))
	gofmt := func(src string) string {
		out, err := format.Source([]byte(src))
		if err != nil {
			log.Fatal(err)
		}
		return string(out)
	}
	files := map[string]string{
		"src/wire.gen.hpp": cpp(s),
		"src/style.gen.inc": cppStyle(s),
		"src/node.gen.hpp": nodeCpp(s),
		"wire_gen.go":      gofmt(goSource(s)),
		"style_gen.go":     gofmt(goStyle(s)),
		"web/wire.gen.js":  js(s),
		"docs/wire.md":     replaceRegion(read("docs/wire.md"), "ops", wireDoc(s)),
		"SKILL.md":         replaceRegion(read("SKILL.md"), "fields", skillDoc(s)),
		"docs/cpp.md":      cppDoc(s, read("src/qui.hpp")),
	}
	names := make([]string, 0, len(files))
	for name := range files {
		names = append(names, name)
	}
	sort.Strings(names)
	stale := 0
	for _, name := range names {
		path := filepath.Join(root, name)
		if *check {
			if current, err := os.ReadFile(path); err != nil || !bytes.Equal(current, []byte(files[name])) {
				fmt.Fprintf(os.Stderr, "gen: %s is stale; run go run ./tools/gen\n", name)
				stale++
			}
			continue
		}
		if err := os.WriteFile(path, []byte(files[name]), 0o644); err != nil {
			log.Fatal(err)
		}
	}
	if stale > 0 {
		os.Exit(1)
	}
}
