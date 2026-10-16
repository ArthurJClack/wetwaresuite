#include <cmath>
#include <filesystem>

#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/document.hpp"
#include "achclwss/raster/ops.hpp"
#include "achclwss/raster/ora.hpp"
#include "achclwss/raster/paint.hpp"
#include "check.hpp"

using namespace achclwss::raster;
namespace fs = std::filesystem;

namespace {

double alphaSum(const Image& img) {
    double s = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) s += img.at(x, y).a / 255.0;
    }
    return s;
}

}  // namespace

int main() {
    // colours
    CHECK(Color::parse("#ff0000") == (Color{255, 0, 0, 255}));
    CHECK(Color::parse("#0f08") == (Color{0, 255, 0, 136}));
    CHECK(Color::parse("rgb(1, 2, 3)") == (Color{1, 2, 3, 255}));
    CHECK(Color::parse("rgba(10,20,30,0.5)")->a == 128);
    CHECK(Color::parse("Transparent")->a == 0);
    CHECK(!Color::parse("#12").has_value());
    CHECK(!Color::parse("nope").has_value());
    CHECK(Color{1, 2, 3, 4}.hex() == "#01020304");

    // compositing
    Image base(2, 1, {0, 0, 255, 255});
    Image top(2, 1, {255, 0, 0, 255});
    blendOnto(base, top, 0, 0, Blend::Normal, 0.5);
    CHECK(std::abs(base.at(0, 0).r - 128) <= 1 && std::abs(base.at(0, 0).b - 127) <= 1);
    Image white(1, 1, {255, 255, 255, 255});
    blendOnto(white, Image(1, 1, {100, 100, 100, 255}), 0, 0, Blend::Multiply, 1.0);
    CHECK(white.at(0, 0).r == 100);
    Image clear(1, 1);
    blendOnto(clear, Image(1, 1, {10, 20, 30, 255}), 0, 0, Blend::Multiply, 1.0);
    CHECK(clear.at(0, 0) == (Color{10, 20, 30, 255}));  // nothing under it: source shows as is

    // geometry
    Image grad(40, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 40; ++x) grad.set(x, y, {static_cast<std::uint8_t>(x * 6), static_cast<std::uint8_t>(y * 12), 0, 255});
    }
    CHECK(resize(grad, 10, 5).width() == 10);
    CHECK(resize(grad, 400, 200).height() == 200);
    CHECK(resize(grad, 7, 3, Filter::Nearest).at(0, 0) == grad.at(0, 0));
    CHECK(crop(grad, 30, 10, 50, 50).width() == 10);
    CHECK(rotate90(grad, 1).width() == 20 && rotate90(grad, 1).at(19, 0) == grad.at(0, 0));
    CHECK(rotate90(grad, 2).at(0, 0) == grad.at(39, 19));
    CHECK(flipHorizontal(grad).at(0, 0) == grad.at(39, 0));
    CHECK(flipVertical(grad).at(0, 0) == grad.at(0, 19));
    CHECK(resizeCanvas(grad, 60, 20, 0, 0).at(45, 0).a == 0);

    // adjustments
    Image gray(1, 1, {100, 100, 100, 255});
    adjust(gray, Adjust::Brightness, 20);
    CHECK(gray.at(0, 0).r == 151);
    Image colourful(1, 1, {200, 50, 50, 255});
    adjust(colourful, Adjust::Saturation, -100);
    CHECK(colourful.at(0, 0).r == colourful.at(0, 0).g);
    Image hue(1, 1, {255, 0, 0, 255});
    adjust(hue, Adjust::Hue, 120);
    CHECK(hue.at(0, 0).g == 255 && hue.at(0, 0).r == 0);
    Image inv(1, 1, {0, 10, 255, 77});
    applyEffect(inv, Effect::Invert, 0);
    CHECK(inv.at(0, 0) == (Color{255, 245, 0, 77}));
    Image blurred = grad;
    applyEffect(blurred, Effect::Blur, 6);
    CHECK(blurred.at(20, 10).r > 0 && blurred.at(0, 0).r != grad.at(39, 0).r);
    CHECK(parseAdjust("contrast").has_value() && !parseAdjust("x").has_value());

    // painting
    Image canvas(100, 100);
    Brush brush;
    brush.color = {255, 0, 0, 255};
    brush.size = 10;
    brush.hardness = 1.0;
    stroke(canvas, brush, {{10, 50, 1}, {90, 50, 1}});
    CHECK(canvas.at(50, 50).a == 255 && canvas.at(50, 50).r == 255);
    CHECK(canvas.at(50, 10).a == 0);
    double painted = alphaSum(canvas);
    CHECK(painted > 800 && painted < 1000);  // ~80 x 10 + round caps
    brush.erase = true;
    stroke(canvas, brush, {{50, 50, 1}});
    CHECK(canvas.at(50, 50).a == 0);

    Image half(10, 10);
    brush.erase = false;
    brush.opacity = 0.5;
    stroke(half, brush, {{5, 5, 1}, {5, 5, 1}, {5, 5, 1}});
    CHECK(std::abs(half.at(5, 5).a - 128) <= 1);  // repeated dabs never exceed stroke opacity

    Image box(20, 20, {255, 255, 255, 255});
    fillRect(box, 5, 5, 10, 10, {0, 0, 0, 255});
    CHECK(box.at(10, 10) == (Color{0, 0, 0, 255}));
    CHECK(box.at(2, 2) == (Color{255, 255, 255, 255}));
    Image poly(20, 20);
    fillRect(poly, 2.5, 2.5, 10, 10, {0, 0, 0, 255});
    CHECK(std::abs(alphaSum(poly) - 100) < 1.0);  // antialiased area is exact
    Image circle(50, 50);
    fillEllipse(circle, 25, 25, 20, 20, {0, 0, 255, 255});
    CHECK(std::abs(alphaSum(circle) - 3.14159 * 400) < 15);

    Image fill(10, 10, {255, 255, 255, 255});
    drawLine(fill, 5, 0, 5, 10, {0, 0, 0, 255}, 1);
    CHECK(floodFill(fill, 0, 0, {255, 0, 0, 255}, 10) > 30);
    CHECK(fill.at(0, 0) == (Color{255, 0, 0, 255}));
    CHECK(fill.at(9, 9) == (Color{255, 255, 255, 255}));  // other side of the line untouched

    // files
    auto dir = fs::temp_directory_path() / "achclwss_test_raster";
    fs::create_directories(dir);
    save(grad, dir / "grad.png");
    Image back = load(dir / "grad.png");
    CHECK(back.width() == 40 && back.at(7, 3) == grad.at(7, 3));
    save(grad, dir / "grad.jpg", 95);
    CHECK(load(dir / "grad.jpg").width() == 40);
    save(grad, dir / "grad.bmp");
    save(grad, dir / "grad.tga");
    CHECK(load(dir / "grad.tga").at(1, 1) == grad.at(1, 1));
    save(grad, dir / "grad.ppm");
    CHECK(load(dir / "grad.ppm").at(5, 5).r == grad.at(5, 5).r);
    bool threw = false;
    try {
        load(dir / "missing.png");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    auto zip = writeZip({{"a.txt", "hello"}, {"dir/b.bin", std::string("\0\1\2", 3)}});
    auto entries = readZip(zip);
    CHECK(entries.size() == 2 && entries[0].data == "hello" && entries[1].name == "dir/b.bin");

    LayeredImage layered{40, 20, {}};
    layered.layers.push_back({"Background", grad});
    layered.layers.push_back({"Ink & \"notes\"", canvas.empty() ? grad : resize(canvas, 40, 20), 0.5, Blend::Multiply, false});
    saveOra(layered, dir / "test.ora");
    LayeredImage ora = loadOra(dir / "test.ora");
    CHECK(ora.width == 40 && ora.height == 20);
    CHECK(ora.layers.size() == 2);
    CHECK(ora.layers[0].name == "Background");
    CHECK(ora.layers[1].name == "Ink & \"notes\"");
    CHECK(ora.layers[1].blend == Blend::Multiply && !ora.layers[1].visible);
    CHECK(std::abs(ora.layers[1].opacity - 0.5) < 1e-6);
    CHECK(ora.layers[0].image.at(3, 3) == grad.at(3, 3));

    auto doc = openImage(dir / "test.ora");
    CHECK(doc->image.layers.size() == 2 && doc->active == 1);
    CHECK(doc->preview()->bytes.size() > 100);
    saveImage(*doc, dir / "flat.png");
    CHECK(load(dir / "flat.png").width() == 40);

    fs::remove_all(dir);
    return check::summary("raster");
}
