// Create-match pictures for the mod map slots: the id221 Flash edit (standard C++ only, so it can be tested off
// the game).
//
// The map picture is a clip (sprite 228 in the stock id221) whose frame map + 2 places that map's shape, a fill of
// the external image ID221_SC<nn>_ID (GFx tag 1001: id, format, width, height, name, file): frames 2-14 = maps 0-12
// (SC00-SC12), frame 17 = map 15 (SC13); frames 15 / 16 (maps 13 / 14, our slots) show nothing. We add the images
// ID221_SC14_ID / ID221_SC15_ID and two shapes like SC13's, and give frames 15 / 16 those shapes (frame 17 first
// removes what's there, as it now follows a picture). The textures go into the archive beside it (packages.cpp).

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

namespace xls::mods {

namespace {

struct Tag {
	uint32_t code;
	size_t at, body, length; // the tag's start, its body, the body's length
};

bool NextTag(const std::vector<uint8_t>& f, size_t at, size_t end, Tag* t)
{
	if (at + 2 > end) {
		return false;
	}
	uint16_t head = (uint16_t)(f[at] | f[at + 1] << 8);
	t->code = head >> 6;
	t->length = head & 0x3f;
	t->at = at;
	t->body = at + 2;
	if (t->length == 0x3f) {
		if (at + 6 > end) {
			return false;
		}
		memcpy(&t->length, &f[at + 2], 4);
		t->body = at + 6;
	}
	return t->body + t->length <= end;
}

uint16_t U16(const std::vector<uint8_t>& f, size_t at)
{
	return (uint16_t)(f[at] | f[at + 1] << 8);
}

void Put16(std::vector<uint8_t>* f, size_t at, uint16_t v)
{
	(*f)[at] = (uint8_t)v;
	(*f)[at + 1] = (uint8_t)(v >> 8);
}

std::vector<uint8_t> MakeTag(uint32_t code, const std::vector<uint8_t>& body)
{
	std::vector<uint8_t> t;
	uint16_t h = (uint16_t)(code << 6 | 0x3f); // long form, always
	t.push_back((uint8_t)h);
	t.push_back((uint8_t)(h >> 8));
	uint32_t n = (uint32_t)body.size();
	for (int k = 0; k < 4; k++) {
		t.push_back((uint8_t)(n >> (k * 8)));
	}
	t.insert(t.end(), body.begin(), body.end());
	return t;
}

const uint32_t kShowFrame = 1, kPlace2 = 26, kRemove2 = 28, kSprite = 39, kExternalImage = 1001;
bool IsShape(uint32_t code)
{
	return code == 2 || code == 22 || code == 32 || code == 83;
}

// An external image tag's name (after id u16, format u16, width u16, height u16: u8 length + text).
std::string ImageName(const std::vector<uint8_t>& f, const Tag& t)
{
	if (t.length < 9) {
		return "";
	}
	size_t n = f[t.body + 8];
	return n + 9 <= t.length ? std::string((const char*)&f[t.body + 9], n) : "";
}

// The image tag's body with another id and name (the file name follows the same way: "..\dds\<name>.dds").
std::vector<uint8_t> RenamedImage(const std::vector<uint8_t>& f, const Tag& t, uint16_t id, const std::string& name)
{
	std::vector<uint8_t> b(f.begin() + t.body, f.begin() + t.body + 8);
	Put16(&b, 0, id);
	std::string file = "..\\dds\\" + name + ".dds";
	b.push_back((uint8_t)name.size());
	b.insert(b.end(), name.begin(), name.end());
	b.push_back((uint8_t)file.size());
	b.insert(b.end(), file.begin(), file.end());
	size_t n = f[t.body + 8], fileAt = t.body + 9 + n; // anything after the stock file name stays
	if (fileAt < t.body + t.length) {
		size_t fn = f[fileAt];
		size_t rest = fileAt + 1 + fn;
		if (rest < t.body + t.length) {
			b.insert(b.end(), f.begin() + rest, f.begin() + t.body + t.length);
		}
	}
	return b;
}

// Where a shape's bitmap fill names the image (fill type 0x40-0x43, then the bitmap id), or 0.
size_t BitmapRef(const std::vector<uint8_t>& f, const Tag& t, uint16_t image)
{
	for (size_t k = t.body + 2; k + 3 <= t.body + t.length; k++) {
		if (f[k] >= 0x40 && f[k] <= 0x43 && U16(f, k + 1) == image) {
			return k + 1;
		}
	}
	return 0;
}

}

// 1: patched; 0: nothing to do (already patched); -1: the Flash isn't laid out as expected.
int ThumbsPatchMenuFlash(std::vector<uint8_t>* gfx)
{
	std::vector<uint8_t>& f = *gfx;
	if (f.size() < 16 || memcmp(f.data(), "GFX", 3) != 0) {
		return -1;
	}
	size_t start = 8 + (5 + 4 * (f[8] >> 3) + 7) / 8 + 4;
	Tag t;
	Tag image13{}, shape13{}, sprite{};
	bool haveImage = false, haveShape = false, haveSprite = false;
	uint16_t imageId = 0, shapeId = 0, maxId = 0;
	for (size_t at = start; NextTag(f, at, f.size(), &t) && t.code != 0; at = t.body + t.length) {
		if (t.code == kExternalImage || IsShape(t.code) || t.code == kSprite) {
			if (t.length >= 2 && U16(f, t.body) > maxId) {
				maxId = U16(f, t.body);
			}
		}
		if (t.code == kExternalImage) {
			std::string n = ImageName(f, t);
			if (n == "ID221_SC14_ID") {
				return 0;
			}
			if (n == "ID221_SC13_ID") {
				image13 = t;
				imageId = U16(f, t.body);
				haveImage = true;
			}
		} else if (IsShape(t.code) && haveImage && !haveShape && BitmapRef(f, t, imageId)) {
			shape13 = t;
			shapeId = U16(f, t.body);
			haveShape = true;
		} else if (t.code == kSprite && haveShape && !haveSprite) {
			// the clip placing that shape
			Tag n;
			for (size_t q = t.body + 4; NextTag(f, q, t.body + t.length, &n) && n.code != 0; q = n.body + n.length) {
				if (n.code == kPlace2 && n.length >= 5 && (f[n.body] & 2) && U16(f, n.body + 3) == shapeId) {
					sprite = t;
					haveSprite = true;
					break;
				}
			}
		}
	}
	if (!haveSprite) {
		return -1;
	}
	// the clip's frames: tags up to and including each ShowFrame
	std::vector<std::vector<std::vector<uint8_t>>> frames(1);
	std::vector<uint8_t> place13;
	int frame13 = -1;
	Tag n;
	size_t q = sprite.body + 4;
	for (; NextTag(f, q, sprite.body + sprite.length, &n) && n.code != 0; q = n.body + n.length) {
		std::vector<uint8_t> raw(f.begin() + n.at, f.begin() + n.body + n.length);
		if (n.code == kPlace2 && n.length >= 5 && (f[n.body] & 2) && U16(f, n.body + 3) == shapeId) {
			place13 = raw;
			frame13 = (int)frames.size() - 1;
		}
		frames.back().push_back(raw);
		if (n.code == kShowFrame) {
			frames.emplace_back();
		}
	}
	std::vector<uint8_t> endTag(f.begin() + n.at, f.begin() + n.body + n.length); // End
	if (frames.back().empty()) {
		frames.pop_back();
	}
	if (frame13 != 16 || frames.size() < 17 || place13.empty()) {
		return -1; // not the stock layout (SC13 on frame 17)
	}
	// PlaceObject2 raw: header (2 or 6), flags, depth u16, character u16
	size_t head = ((U16(place13, 0) & 0x3f) == 0x3f) ? 6 : 2;
	uint16_t depth = U16(place13, head + 1);
	auto placeOf = [&](uint16_t id) {
		std::vector<uint8_t> p = place13;
		Put16(&p, head + 3, id);
		return p;
	};
	auto removeAt = [&]() {
		std::vector<uint8_t> r = {(uint8_t)(kRemove2 << 6 | 2), (uint8_t)((kRemove2 << 6 | 2) >> 8), 0, 0};
		Put16(&r, 2, depth);
		return r;
	};
	uint16_t img14 = (uint16_t)(maxId + 1), img15 = (uint16_t)(maxId + 2);
	uint16_t sh14 = (uint16_t)(maxId + 3), sh15 = (uint16_t)(maxId + 4);
	auto insertBeforeShow = [&](std::vector<std::vector<uint8_t>>& fr, const std::vector<uint8_t>& tag) {
		fr.insert(fr.end() - 1, tag);
	};
	// frame 15 (map 13): the stock removes the picture there; ours goes in after
	insertBeforeShow(frames[14], placeOf(sh14));
	// frame 16 (map 14): off with map 13's, ours in
	insertBeforeShow(frames[15], removeAt());
	insertBeforeShow(frames[15], placeOf(sh15));
	// frame 17 (map 15): off with map 14's before SC13 goes in
	frames[16].insert(frames[16].begin(), removeAt());
	// the new definitions: images like SC13's, shapes like its
	std::vector<uint8_t> defs;
	for (int k = 0; k < 2; k++) {
		std::vector<uint8_t> ib = RenamedImage(f, image13, k ? img15 : img14, k ? "ID221_SC15_ID" : "ID221_SC14_ID");
		std::vector<uint8_t> it = MakeTag(kExternalImage, ib);
		defs.insert(defs.end(), it.begin(), it.end());
	}
	for (int k = 0; k < 2; k++) {
		std::vector<uint8_t> sb(f.begin() + shape13.body, f.begin() + shape13.body + shape13.length);
		Put16(&sb, 0, k ? sh15 : sh14);
		size_t ref = BitmapRef(f, shape13, imageId) - shape13.body;
		Put16(&sb, ref, k ? img15 : img14);
		std::vector<uint8_t> st = MakeTag(shape13.code, sb);
		defs.insert(defs.end(), st.begin(), st.end());
	}
	// the clip again
	std::vector<uint8_t> body(f.begin() + sprite.body, f.begin() + sprite.body + 4);
	for (auto& fr : frames) {
		for (auto& tag : fr) {
			body.insert(body.end(), tag.begin(), tag.end());
		}
	}
	body.insert(body.end(), endTag.begin(), endTag.end());
	std::vector<uint8_t> spriteTag = MakeTag(kSprite, body);
	std::vector<uint8_t> out(f.begin(), f.begin() + sprite.at);
	out.insert(out.end(), defs.begin(), defs.end());
	out.insert(out.end(), spriteTag.begin(), spriteTag.end());
	out.insert(out.end(), f.begin() + sprite.body + sprite.length, f.end());
	uint32_t size = (uint32_t)out.size();
	memcpy(&out[4], &size, 4);
	f.swap(out);
	return 1;
}

}
