/*=============================================================================
   Copyright (c) 2016-2020 Joel de Guzman

   Distributed under the MIT License [ https://opensource.org/licenses/MIT ]
=============================================================================*/
#include <string_view>
#include <string>
#include <artist/text_layout.hpp>
#include <artist/canvas.hpp>
#include <artist/unicode.hpp>
#include <vector>
#include <SkFont.h>
#include <SkTextBlob.h>
#include <SkCanvas.h>
#include "detail/harfbuzz.hpp"
#include "linebreak.h"
#include "opaque.hpp"

namespace cycfi::artist
{
   class text_layout::impl
   {
   public:

      impl(font const& font_, std::string_view utf8);
      impl(font const& font_, color c, std::string_view utf8);
      ~impl();

      struct row_info
      {
         point                   pos;
         float                   width;
         float                   height;
         std::size_t             glyph_count;
         std::size_t             glyph_index;
         sk_sp<SkTextBlob>       line;
         std::vector<SkScalar>   positions;
      };

      void                       flow(get_line_info const& glf, flow_info finfo);
      void                       draw(canvas& cnv, point p);
      void                       text(std::string_view utf8);
      point                      caret_point(std::size_t index) const;
      std::size_t                caret_index(point p) const;
      std::size_t                num_lines() const;
      class font&                get_font();

   private:

      using line_vector = std::vector<row_info>;

      class font              _font;
      color                   _text_color;
      detail::hb_font         _hb_font;
      std::string_view        _utf8;
      detail::hb_buffer       _buff;
      line_vector             _rows;
      SkPaint                 _paint;
   };

   text_layout::impl::impl(class font const& font_, color c, std::string_view utf8)
    : _font{ font_ }
    , _hb_font(_font.impl()->getTypeface())
    , _text_color{ c }
    , _utf8{ utf8 }
    , _buff{ utf8 }
   {
      // $$$ For now... later, this should be extracted from the text, perhaps?
      _buff.direction(HB_DIRECTION_LTR);
      _buff.script(HB_SCRIPT_LATIN);
      _buff.language("en");

      init_linebreak();

      _paint.setAntiAlias(true);
      _paint.setStyle(SkPaint::kFill_Style);
      _paint.setColor4f({ c.red, c.green, c.blue, c.alpha }, nullptr);
   }

   text_layout::impl::impl(class font const& font_, std::string_view utf8)
    : impl{ font_, colors::black, utf8 }
   {}

   text_layout::impl::~impl()
   {
   }

   void text_layout::impl::flow(get_line_info const& glf, flow_info finfo)
   {
      _rows.clear();
      if (_utf8.size() == 0)
         return;

      _buff.shape(_hb_font);
      auto glyphs_info = _buff.glyphs();

      int hb_scalex, hb_scaley;
      hb_font_get_scale(_hb_font.get(), &hb_scalex, &hb_scaley);
      auto sc_font = _font.impl();
      float scalex = (sc_font->getSize() / hb_scalex) * sc_font->getScaleX();

      std::vector<SkScalar> positions;
      positions.reserve(glyphs_info.count);
      auto linfo = glf(0);
      float y = 0;
      float x = 0;
      auto glyph_start = 0;

      std::string brks(_utf8.size(), 0);
      set_linebreaks_utf8(
         (utf8_t const*)_utf8.data()
         , _utf8.size(), _buff.language(), brks.data()
      );

      auto num_spaces =
         [&](std::size_t glyph_idx) -> std::size_t
         {
            std::size_t count = 0;
            for (auto i = glyph_start; i != glyph_idx; ++i)
            {
               auto cl = glyphs_info.glyphs[i].cluster;
               if (unicode::is_space(&_utf8[cl]))
                  ++count;
            }
            return count;
         };

      auto justify =
         [&](std::size_t glyph_idx, bool must_break) -> float
         {
            auto line_width =
               positions[glyph_idx-glyph_start] +
               (glyphs_info.positions[glyph_idx].x_advance * scalex)
            ;
            if (finfo.justify && !must_break)
            {
               if (((line_width / linfo.width) > 0.9))
               {
                  // Full justify
                  auto nspaces = num_spaces(glyph_idx);
                  float extra = (linfo.width - line_width) / nspaces;
                  float offset = 0;
                  for (auto i = glyph_start; i != glyph_idx; ++i)
                  {
                     using namespace unicode;
                     auto cl = glyphs_info.glyphs[i].cluster;
                     if (unicode::is_space(&_utf8[cl]))
                        offset += extra;
                     positions[i-glyph_start] += offset;
                  }
                  if (glyph_idx < positions.size())
                     positions[glyph_idx] += offset;
                  line_width = linfo.width;
               }
            }
            return line_width;
         };

      auto new_line =
         [&](std::size_t utf8_idx, std::size_t& i, bool must_break)
         {
            auto glyph_idx = _buff.glyph_index(utf8_idx);
            auto line_width = justify(glyph_idx, must_break);

            auto glyph_count = glyph_idx - glyph_start;
            if (i == glyphs_info.count-1) // the last glyph?
               ++glyph_count;

            std::vector<SkGlyphID> line_glyphs(glyph_count);
            for (auto j = 0; j != glyph_count; ++j)
               line_glyphs[j] = glyphs_info.glyphs[glyph_start + j].codepoint;

            auto text_blob = SkTextBlob::MakeFromPosTextH(
               line_glyphs.data()
               , line_glyphs.size() * sizeof(SkGlyphID)
               , positions.data(), 0
               , *_font.impl()
               , SkTextEncoding::kGlyphID
            );

            auto last_glyph = std::min<std::size_t>(glyph_count+1, positions.size());
            positions.erase(positions.begin()+last_glyph, positions.end());
            _rows.push_back(
               row_info{
                  point{ linfo.offset, y }
                  , line_width
                  , finfo.line_height
                  , std::size_t(glyph_count)
                  , std::size_t(glyph_start)
                  , text_blob
                  , std::move(positions)
               }
            );

            positions.clear();
            i = glyph_idx;
            glyph_start = glyph_idx + 1;
            y += finfo.line_height;
            linfo = glf(y);
            x = 0;
         };

      for (std::size_t i = 0; i != glyphs_info.count; ++i)
      {
         positions.push_back(x + (glyphs_info.positions[i].x_offset * scalex));
         x += glyphs_info.positions[i].x_advance * scalex;

         if (auto idx = glyphs_info.glyphs[i].cluster; brks[idx] == LINEBREAK_MUSTBREAK)
         {
            // We must break now
            new_line(idx, i, true);
         }
         else if (x > linfo.width)
         {
            // We break the line when x exceeds the target width
            std::size_t len = positions.size();
            auto start_line = glyphs_info.glyphs[glyph_start].cluster;
            auto end_line = glyphs_info.glyphs[glyph_start+len].cluster;
            auto brks_len = end_line-start_line;
            auto brks_line = brks.substr(start_line, brks_len);
            auto pos = brks_line.find_last_of(char(LINEBREAK_ALLOWBREAK), brks_len-1);

            if (pos != brks_line.npos)
               new_line(pos + start_line, i, false);
            else
               ; // deal with the case where we have to forcefully break the line
         }
      }
      auto& last = _rows.back();
      last.pos.y += finfo.last_line_height - finfo.line_height;
      last.height = finfo.last_line_height;
   }

   void  text_layout::impl::draw(canvas& cnv, point p)
   {
      if (_rows.size() == 0)
         return;

      for (auto const& line : _rows)
      {
         auto sk_cnv = cnv.impl();
         SkPaint paint;
         paint.setAntiAlias(true);
         paint.setStyle(SkPaint::kFill_Style);
         sk_cnv->drawTextBlob(
            line.line
            , p.x+line.pos.x, p.y+line.pos.y
            , _paint
         );
      }
   }

   void text_layout::impl::text(std::string_view utf8)
   {
      if (utf8 != _utf8)
      {
         _buff.text(utf8);
         _rows.clear();
      }
   }

   point text_layout::impl::caret_point(std::size_t index) const
   {
      if (_rows.size() == 0)
         return { 0, 0 };

      auto glyphs_info = _buff.glyphs();

      // Find the glyph index from string index
      auto glyph_index = index;
      auto row_index = -1;
      if (index < _utf8.size())
      {
         glyph_index = _buff.glyph_index(index);
      }
      else
      {
         glyph_index = glyphs_info.count;
         row_index = _rows.size() - 1;
      }

      // Find the row that includes the glyph index
      if (row_index == -1)
      {
         auto i = std::lower_bound(_rows.begin(), _rows.end(), glyph_index,
            [](auto const& row, std::size_t pos)
            {
               return (row.glyph_index + row.glyph_count) < pos;
            }
         );
         if (i == _rows.end())
            return { -1, -1 };
         row_index = i - _rows.begin();
      }

      // Now find the glyph position in the row
      auto const& row = _rows[row_index];
      auto pos = glyph_index - row.glyph_index;
      auto offset = (pos < row.positions.size())? row.positions[pos] : row.width;
      return { row.pos.x + offset, row.pos.y };
   }

   std::size_t text_layout::impl::caret_index(point p) const
   {
      if (_rows.size() == 0)
         return 0;

      auto glyphs_info = _buff.glyphs();
      auto i = std::lower_bound(
         _rows.begin(), _rows.end(), p.y,
         [](auto const& row, float y)
         {
            return row.pos.y < y;
         }
      );
      if (i == _rows.end())
         return npos;

      if (p.x <= i->pos.x)
         return glyphs_info.glyphs[i->glyph_index].cluster;

      auto is_last_row = (i == _rows.end()-1);
      if (!is_last_row && p.x >= (i->pos.x + i->width))
         return glyphs_info.glyphs[i->glyph_index + i->glyph_count].cluster;

      auto f = i->positions.begin();
      auto l = i->positions.end();
      auto j = std::lower_bound(f, l, p.x - i->pos.x,
         [](float pos, float x)
         {
            return pos < x;
         }
      );
      if (j == l)
         return is_last_row? _utf8.size() : npos;
      auto index = i->glyph_index + (j-f);
      return glyphs_info.glyphs[index].cluster;
   }

   std::size_t text_layout::impl::num_lines() const
   {
      return _rows.size();
   }

   class font& text_layout::impl::get_font()
   {
      return _font;
   }

   text_layout::text_layout(font const& font_, std::string_view utf8)
    : _impl{ std::make_unique<impl>(font_, utf8) }
   {
   }

   text_layout::text_layout(font const& font_, color c, std::string_view utf8)
    : _impl{ std::make_unique<impl>(font_, c, utf8) }
   {
   }

   text_layout::~text_layout()
   {
   }

   text_layout::text_layout(text_layout&& rhs) noexcept
    : _impl{ std::move(rhs._impl) }
   {
   }

   void text_layout::text(std::string_view utf8)
   {
      _impl->text(utf8);
   }

   void text_layout::flow(float width, bool justify)
   {
      auto line_info_f = [width](float /*y*/)
      {
         return line_info{ 0, width };
      };

      auto lh = _impl->get_font().line_height();
      flow(line_info_f, { justify, lh, lh });
   }

   void text_layout::flow(get_line_info const& glf, flow_info finfo)
   {
      _impl->flow(glf, finfo);
   }

   void text_layout::draw(canvas& cnv, point p) const
   {
      _impl->draw(cnv, p);
   }

   std::size_t text_layout::num_lines() const
   {
      return _impl->num_lines();
   }


   point text_layout::caret_point(std::size_t index) const
   {
      return _impl->caret_point(index);
   }

   std::size_t text_layout::caret_index(point p) const
   {
      return _impl->caret_index(p);
   }
}

