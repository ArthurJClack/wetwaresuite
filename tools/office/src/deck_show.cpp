// Slide decks for presenting: notes pages (PDF) and a self-contained HTML slide show
// with transitions, timings, speaker notes and a presenter window.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "wetwaresuite/office/deck.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "text.hpp"
#include "textlayout.hpp"

namespace wetwaresuite::office {

namespace {

// Copies a page's drawing into `dst`, scaled by k and moved by (dx, dy).
void appendScaled(pdf::Page& dst, const pdf::Page& src, double k, double dx, double dy) {
    for (const auto& op : src.ops) {
        if (auto t = std::get_if<pdf::TextOp>(&op)) {
            pdf::TextOp c = *t;
            c.x = dx + t->x * k, c.y = dy + t->y * k, c.size = t->size * k;
            dst.ops.emplace_back(std::move(c));
        } else if (auto r = std::get_if<pdf::RectOp>(&op)) {
            pdf::RectOp c = *r;
            c.x = dx + r->x * k, c.y = dy + r->y * k, c.w = r->w * k, c.h = r->h * k, c.lineWidth = r->lineWidth * k;
            dst.ops.emplace_back(c);
        } else if (auto l = std::get_if<pdf::LineOp>(&op)) {
            pdf::LineOp c = *l;
            c.x1 = dx + l->x1 * k, c.y1 = dy + l->y1 * k, c.x2 = dx + l->x2 * k, c.y2 = dy + l->y2 * k, c.width = l->width * k;
            dst.ops.emplace_back(c);
        } else if (auto im = std::get_if<pdf::ImageOp>(&op)) {
            pdf::ImageOp c = *im;
            c.x = dx + im->x * k, c.y = dy + im->y * k, c.w = im->w * k, c.h = im->h * k;
            dst.ops.emplace_back(c);
        } else if (auto p = std::get_if<pdf::PathOp>(&op)) {
            pdf::PathOp c = *p;
            for (auto& [x, y] : c.points) x = dx + x * k, y = dy + y * k;
            c.lineWidth = p->lineWidth * k;
            dst.ops.emplace_back(std::move(c));
        }
    }
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\') out += '\\', out += ch;
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') continue;
        else if (ch == '\t') out += "\\t";
        else if (ch == '<') out += "\\u003c";
        else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
        } else out += ch;
    }
    return out + "\"";
}

std::string htmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

bool hasPicture(const Deck& d, const Slide& s) {
    auto pic = [](const SlideObject& o) { return o.kind == "image"; };
    const bool master = d.masterOnTitle || (s.layout != "title" && s.layout != "section");
    return (s.pixels && !s.image.empty()) || std::any_of(s.objects.begin(), s.objects.end(), pic) ||
           (master && std::any_of(d.master.begin(), d.master.end(), pic));
}

const char* kShowCss = R"CSS(
html,body{margin:0;height:100%;background:#000;overflow:hidden;font-family:system-ui,-apple-system,"Segoe UI",sans-serif}
#stage{position:fixed;inset:0;display:flex;align-items:center;justify-content:center}
#frame{position:relative;overflow:hidden;background:#000}
.slide{position:absolute;inset:0;visibility:hidden}
.slide.on{visibility:visible}
.slide img{width:100%;height:100%;display:block;user-select:none;-webkit-user-drag:none}
#notes{position:fixed;left:0;right:0;bottom:0;max-height:35%;overflow:auto;background:rgba(18,18,20,.94);color:#eee;padding:14px 22px;font-size:18px;line-height:1.45;white-space:pre-wrap;display:none;z-index:10}
#bar{position:fixed;right:14px;bottom:10px;color:#aaa;font-size:13px;z-index:11;opacity:0;transition:opacity .4s}
#bar.show{opacity:1}
#cover{position:fixed;inset:0;display:none;z-index:20}
#help{position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);background:#1d1e22;color:#eee;padding:18px 26px;border-radius:10px;font-size:15px;line-height:1.7;display:none;z-index:30;box-shadow:0 8px 40px rgba(0,0,0,.6)}
#help b{display:inline-block;min-width:120px;color:#fff}
)CSS";

const char* kShowJs = R"JS(
(function(){
var slides=[].slice.call(document.querySelectorAll('.slide'));
var info=JSON.parse(document.getElementById('show-data').textContent);
var frame=document.getElementById('frame'),notesEl=document.getElementById('notes'),bar=document.getElementById('bar');
var coverEl=document.getElementById('cover'),help=document.getElementById('help');
var cur=-1,timer=null,typed='',presenter=null,started=Date.now(),barTimer=null;
function fit(){var k=Math.min(innerWidth/info.width,innerHeight/info.height);frame.style.width=info.width*k+'px';frame.style.height=info.height*k+'px';}
addEventListener('resize',fit);fit();
function vec(d){return d==='right'?[-1,0]:d==='up'?[0,1]:d==='down'?[0,-1]:[1,0];}
function pct(x,y){return 'translate('+(x*100)+'%,'+(y*100)+'%)';}
function play(prev,next,t,d,sec){
  var o={duration:sec*1000,easing:'ease-in-out',fill:'none'},v=vec(d),done=null;
  if(t==='fade'){done=next.animate([{opacity:0},{opacity:1}],o);}
  else if(t==='push'){done=next.animate([{transform:pct(v[0],v[1])},{transform:'none'}],o);prev.animate([{transform:'none'},{transform:pct(-v[0],-v[1])}],o);}
  else if(t==='cover'){done=next.animate([{transform:pct(v[0],v[1])},{transform:'none'}],o);}
  else if(t==='wipe'){var s=d==='right'?'inset(0 100% 0 0)':d==='up'?'inset(100% 0 0 0)':d==='down'?'inset(0 0 100% 0)':'inset(0 0 0 100%)';
    done=next.animate([{clipPath:s},{clipPath:'inset(0 0 0 0)'}],o);}
  else if(t==='zoom'){done=next.animate([{transform:'scale(.25)',opacity:0},{transform:'none',opacity:1}],o);}
  return done;
}
function show(i,instant){
  if(i<0||i>=slides.length)return;
  if(i===cur)return;
  var prev=cur>=0?slides[cur]:null,next=slides[i],back=i<cur;
  clearTimeout(timer);
  slides.forEach(function(x){if(x.getAnimations)x.getAnimations().forEach(function(a){a.cancel();});if(x!==prev&&x!==next){x.classList.remove('on');x.style.zIndex=0;}});
  next.classList.add('on');next.style.zIndex=2;
  if(prev){prev.style.zIndex=1;}
  var t=next.dataset.t,anim=null;
  if(prev&&t&&!instant&&!back&&next.animate&&!matchMedia('(prefers-reduced-motion: reduce)').matches)anim=play(prev,next,t,next.dataset.d,parseFloat(next.dataset.s)||0.75);
  if(anim&&prev){anim.onfinish=function(){if(slides[cur]!==prev)prev.classList.remove('on');};}
  else if(prev)prev.classList.remove('on');
  cur=i;
  try{history.replaceState(null,'','#'+next.dataset.n);}catch(e){}
  notesEl.textContent=info.notes[i]||'(no notes)';
  bar.textContent=next.dataset.n+' / '+info.count;flash();
  var a=parseFloat(next.dataset.a)||0;
  if(a>0)timer=setTimeout(function(){if(cur<slides.length-1)go(1);else if(info.loop)show(0);},a*1000);
  updatePresenter();
}
function go(n){show(Math.max(0,Math.min(slides.length-1,cur+n)));}
function flash(){bar.classList.add('show');clearTimeout(barTimer);barTimer=setTimeout(function(){bar.classList.remove('show');},1500);}
function blank(c){if(coverEl.style.display==='block'&&coverEl.style.background===c){coverEl.style.display='none';return;}coverEl.style.background=c;coverEl.style.display='block';}
function byNumber(n){for(var i=0;i<slides.length;i++)if(+slides[i].dataset.n>=n)return i;return slides.length-1;}
function fmt(ms){var s=Math.floor(ms/1000),m=Math.floor(s/60);s%=60;return (m<10?'0':'')+m+':'+(s<10?'0':'')+s;}
function updatePresenter(){
  if(!presenter||presenter.closed)return;
  var d=presenter.document,q=function(id){return d.getElementById(id);};
  if(!q('now'))return;
  q('now').src=slides[cur].querySelector('img').src;
  var nx=slides[cur+1];q('next').src=nx?nx.querySelector('img').src:'';q('next').style.visibility=nx?'visible':'hidden';
  q('pn').textContent=info.notes[cur]||'(no notes)';q('count').textContent='Slide '+slides[cur].dataset.n+' of '+info.count;
}
function openPresenter(){
  presenter=window.open('','wwpresenter','width=1100,height=720');
  if(!presenter){alert('Allow pop-ups to open the presenter window.');return;}
  var d=presenter.document;d.open();
  d.write('<!doctype html><title>Presenter</title><style>body{margin:0;background:#16171a;color:#eee;font-family:system-ui,sans-serif;display:grid;grid-template-columns:3fr 2fr;grid-template-rows:auto 1fr;gap:14px;padding:14px;height:100vh;box-sizing:border-box}'+
    'img{width:100%;border-radius:4px;background:#000}#now{grid-row:1}#side{display:flex;flex-direction:column;gap:10px}#pn{grid-column:1/3;overflow:auto;font-size:22px;line-height:1.5;white-space:pre-wrap;background:#202126;padding:14px 18px;border-radius:6px}'+
    '#clock{font-size:40px;font-variant-numeric:tabular-nums}button{font-size:16px;padding:8px 16px;margin-right:6px}</style>'+
    '<img id="now"><div id="side"><div id="clock">00:00</div><div id="count"></div><div>Next</div><img id="next"><div><button id="pv">Previous</button><button id="nx">Next</button><button id="rs">Reset timer</button></div></div><div id="pn"></div>');
  d.close();
  d.getElementById('nx').onclick=function(){go(1);};d.getElementById('pv').onclick=function(){go(-1);};
  d.getElementById('rs').onclick=function(){started=Date.now();};
  d.addEventListener('keydown',key);
  setInterval(function(){if(presenter&&!presenter.closed){var c=presenter.document.getElementById('clock');if(c)c.textContent=fmt(Date.now()-started);}},500);
  updatePresenter();
}
function key(e){
  var k=e.key;
  if(/^[0-9]$/.test(k)){typed+=k;return;}
  if(k==='Enter'&&typed){show(byNumber(parseInt(typed,10)));typed='';e.preventDefault();return;}
  typed='';
  if(k==='ArrowRight'||k==='ArrowDown'||k==='PageDown'||k===' '||k==='Enter'){if(cur===slides.length-1&&info.loop)show(0);else go(1);}
  else if(k==='ArrowLeft'||k==='ArrowUp'||k==='PageUp'||k==='Backspace')go(-1);
  else if(k==='Home')show(0);
  else if(k==='End')show(slides.length-1);
  else if(k==='f'||k==='F'){if(document.fullscreenElement)document.exitFullscreen();else if(document.documentElement.requestFullscreen)document.documentElement.requestFullscreen();}
  else if(k==='n'||k==='N'){notesEl.style.display=notesEl.style.display==='block'?'none':'block';}
  else if(k==='b'||k==='B'||k==='.')blank('#000');
  else if(k==='w'||k==='W'||k===',')blank('#fff');
  else if(k==='p'||k==='P')openPresenter();
  else if(k==='?'||k==='h'||k==='H'){help.style.display=help.style.display==='block'?'none':'block';}
  else if(k==='Escape'){help.style.display='none';coverEl.style.display='none';}
  else return;
  e.preventDefault();
}
document.addEventListener('keydown',key);
frame.addEventListener('click',function(e){if(coverEl.style.display==='block'){coverEl.style.display='none';return;}go(e.clientX<innerWidth*0.25?-1:1);});
coverEl.addEventListener('click',function(){coverEl.style.display='none';});
var tx=null;
addEventListener('touchstart',function(e){tx=e.touches[0].clientX;},{passive:true});
addEventListener('touchend',function(e){if(tx===null)return;var dx=e.changedTouches[0].clientX-tx;if(Math.abs(dx)>40)go(dx<0?1:-1);tx=null;});
addEventListener('mousemove',flash);
var start=parseInt((location.hash||'').slice(1),10);
show(start>0?byNumber(start):0,true);
})();
)JS";

}  // namespace

std::vector<pdf::Page> layoutNotesPages(const Deck& deck) {
    std::vector<pdf::Page> pages;
    const double W = 540, H = 720, margin = 54;
    const double slideW = W - 2 * margin, k = slideW / deck.width, slideH = deck.height * k;
    const pdf::Font font{deck.theme.font, false, false};
    for (std::size_t i = 0; i < deck.slides.size(); ++i) {
        const Slide& s = deck.slides[i];
        if (s.hidden) continue;
        pdf::Page page;
        page.width = W;
        page.height = H;
        appendScaled(page, layoutSlide(deck, i), k, margin, margin);
        page.rect(margin, margin, slideW, slideH, std::nullopt, pdf::Color{0xBB, 0xBB, 0xBB, 255}, 0.75);
        textlayout::Style st;
        st.font = font;
        st.size = 12;
        double y = margin + slideH + 30;
        auto footer = [&](pdf::Page& p) {
            const std::string n = std::to_string(i + 1);
            p.text(W - margin - pdf::textWidth(font, n, 10), H - 30, n, font, 10, pdf::Color{0x88, 0x88, 0x88, 255});
        };
        for (const auto& line : splitLines(s.notes)) {
            auto lines = textlayout::wrap(textlayout::pieces(parseInline(line), st), W - 2 * margin, 12);
            if (lines.empty()) y += 12 * 1.35;
            for (const auto& l : lines) {
                if (y + l.size * 1.35 > H - margin) {
                    footer(page);
                    pages.push_back(std::move(page));
                    page = pdf::Page{};
                    page.width = W;
                    page.height = H;
                    y = margin;
                }
                textlayout::drawLine(page, l, margin, y + l.size);
                y += l.size * 1.35;
            }
        }
        footer(page);
        pages.push_back(std::move(page));
    }
    return pages;
}

std::string writeSlideShow(const Deck& deck, int width, bool loop) {
    width = std::clamp(width, 320, 3840);
    const double scale = width / deck.width;
    std::string slidesHtml, notes;
    int count = 0;
    for (std::size_t i = 0; i < deck.slides.size(); ++i) {
        const Slide& s = deck.slides[i];
        if (s.hidden) continue;
        ++count;
        raster::Image img = pdf::renderPage(layoutSlide(deck, i), scale);
        const bool photo = hasPicture(deck, s);
        const std::string bytes = photo ? raster::encodeJpeg(img, 90) : raster::encodePng(img);
        char timing[96];
        std::snprintf(timing, sizeof timing, " data-s=\"%.2f\" data-a=\"%.2f\"", s.transitionSeconds, s.advanceAfter);
        slidesHtml += "<div class=\"slide\" data-n=\"" + std::to_string(i + 1) + "\" data-t=\"" + htmlEscape(s.transition) + "\" data-d=\"" +
                      htmlEscape(s.direction) + "\"" + timing + "><img alt=\"" + htmlEscape("Slide " + std::to_string(i + 1) + (s.title.empty() ? "" : ": " + plainText(parseInline(s.title)))) +
                      "\" src=\"data:image/" + (photo ? "jpeg" : "png") + ";base64," + base64Encode(bytes) + "\"></div>\n";
        notes += std::string(notes.empty() ? "" : ",") + jsonString(s.notes);
    }
    char dims[160];
    std::snprintf(dims, sizeof dims, "{\"width\":%.2f,\"height\":%.2f,\"count\":%d,\"loop\":%s,", deck.width, deck.height, count, loop ? "true" : "false");
    const std::string title = deck.title.empty() ? std::string("Slide show") : deck.title;
    std::string html = "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
    html += "<meta name=\"generator\" content=\"wetwaresuite\">\n<title>" + htmlEscape(title) + "</title>\n<style>" + kShowCss + "</style>\n</head>\n<body>\n";
    html += "<div id=\"stage\"><div id=\"frame\">\n" + slidesHtml + "</div></div>\n";
    html += "<div id=\"notes\"></div><div id=\"bar\"></div><div id=\"cover\"></div>\n";
    html += "<div id=\"help\"><b>Next</b> click, Space, &rarr;, Page Down<br><b>Previous</b> &larr;, Page Up, click the left edge<br>"
            "<b>Go to slide</b> type its number, then Enter<br><b>Full screen</b> F<br><b>Speaker notes</b> N<br><b>Presenter window</b> P<br>"
            "<b>Black / white</b> B / W<br><b>This help</b> ?</div>\n";
    html += "<script id=\"show-data\" type=\"application/json\">" + std::string(dims) + "\"notes\":[" + notes + "]}</script>\n";
    html += std::string("<script>") + kShowJs + "</script>\n</body>\n</html>\n";
    return html;
}

}  // namespace wetwaresuite::office
