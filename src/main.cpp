// Distributed under the BSD License, see accompanying LICENSE.txt
// (C) Copyright 2010 John-John Tedro et al.
#include <stdlib.h>
#include <stdio.h>

#include <errno.h>

#include <sstream>
#include <string>
#include <list>
#include <iostream>
#include <iomanip>
#include <fstream>

#include <boost/algorithm/string.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>

#include "config.hpp"

#include "image/format/png.hpp"
#include "image/image_base.hpp"
#include "image/memory_image.hpp"
#include "image/cached_image.hpp"

#include "threads/renderer.hpp"
#include "2d/cube.hpp"

#include "global.hpp"
#include "cache.hpp"
#include "fileutils.hpp"
#include "players.hpp"
#include "text.hpp"
#include "json.hpp"
#include "warps.hpp"

#include "mc/world.hpp"
#include "mc/blocks.hpp"
#include "mc/utils.hpp"

#include "engine/engine_base.hpp"
#include "engine/topdown_engine.hpp"
#include "engine/oblique_engine.hpp"
#include "engine/obliqueangle_engine.hpp"
#include "engine/isometric_engine.hpp"

#include "main_utils.hpp"

using namespace std;
namespace fs = boost::filesystem;

struct nullstream: std::ostream {
  nullstream(): std::ios(0), std::ostream(0) {}
};

nullstream nil;
std::ostream out(std::cout.rdbuf());
std::ofstream out_log;

const uint8_t ERROR_BYTE = 0x01;
const uint8_t RENDER_BYTE = 0x10;
const uint8_t COMP_BYTE = 0x20;
const uint8_t IMAGE_BYTE = 0x30;
const uint8_t PARSE_BYTE = 0x40;
const uint8_t END_BYTE = 0xF0;

struct marker {
public:
  std::string text;
  std::string type;
  text::font_face font;
  int x, y, z;
  
  marker(std::string text, std::string type, text::font_face font, int x, int y, int z) :
      text(text), type(type), font(font), x(x), y(y), z(z)
  {  }
};

struct rotated_level_info {
  typedef boost::shared_ptr<mc::level_info> level_info_ptr;

  level_info_ptr level;
  mc::utils::level_coord coord;
  
  rotated_level_info(level_info_ptr level, mc::utils::level_coord coord)
    : level(level), coord(coord)
  {
  }
  
  bool operator<(const rotated_level_info& other) const {
    return coord < other.coord;
  }
};

void cout_progress_n(image_base::pos_t i, image_base::pos_t all) {
  if (i == all) {
    out << setw(6) << "done!" << endl;
  }
  else {
    if (i % 50 == 0 && i > 0) {
      out << "." << flush;
      
      if (i % 1000 == 0) {
        out << setw(8) << i << " " << (i * 100) / all << "%" << endl;
      }
    }
  } 
}

void cout_progress_ionly_n(image_base::pos_t i, image_base::pos_t all) {
  if (all == 1) {
    out << setw(6) << "done!" << endl;
  }
  else if (i % 50 == 0 && i > 0) {
    out << "." << flush;
    
    if (i % 1000 == 0) {
      out << setw(8) << i << " ?%" << endl;
    }
  } 
}

inline void cout_error(const string& message) {
  cout << hex << std::setw(2) << setfill('0') << static_cast<int>(ERROR_BYTE)
       << hex << message << flush;
}

inline void cout_end() {
  cout << hex << std::setw(2) << setfill('0') << static_cast<int>(END_BYTE) << flush;
}

/*
 * Store part of a level rendered as a small image.
 *
 * This will allow us to composite the entire image later and calculate sizes then.
 */

inline void populate_markers(settings_t& s, json::array* array, boost::shared_ptr<engine_base> engine, boost::ptr_vector<marker>& markers) {
  boost::ptr_vector<marker>::iterator it;
  
  for (it = markers.begin(); it != markers.end(); it++) {
    marker m = *it;
    
    mc::utils::level_coord coord = mc::utils::level_coord(m.x, m.z).rotate(s.rotation);
    
    image_base::pos_t x, y;
    
    engine->wp2pt(coord.get_x(), m.y, coord.get_z(), x, y);

    json::object* o = new json::object;
    
    o->put("text", new json::string(m.text));
    o->put("type", new json::string(m.type));
    
    // the projected coordinates
    o->put("x", new json::number(x));
    o->put("y", new json::number(y));
    
    // the real coordinates
    o->put("X", new json::number(m.x));
    o->put("Y", new json::number(m.y));
    o->put("Z", new json::number(m.z));
    
    array->push(o);
  }
  // don't bother to check for errors right now, but could be done using the "fail" accessor.
}

inline void overlay_markers(settings_t& s, boost::shared_ptr<image_base> all, boost::shared_ptr<engine_base> engine, boost::ptr_vector<marker>& markers) {
  memory_image positionmark(5, 5);
  positionmark.fill(s.ttf_color);
  
  boost::ptr_vector<marker>::iterator it;
  
  for (it = markers.begin(); it != markers.end(); it++) {
    marker m = *it;

    if (!m.font.is_initialized()) {
      continue;
    }
    
    mc::utils::level_coord coord = mc::utils::level_coord(m.x, m.z).rotate(s.rotation);
    
    image_base::pos_t x, y;
    
    engine->wp2pt(coord.get_x(), m.y, coord.get_z(), x, y);
    
    m.font.draw(*all, m.text, x + 5, y);
    //all->safe_composite(x - 3, y - 3, positionmark);
  }
}

template<typename T>
void cout_dot(T total) {
  if (total == 0) out << " done!";
  else out << "." << flush;
}

void cout_uint_endl(unsigned int total) {
  out << " " << setw(8) << total << " parts" << endl;
}

void cout_uintpart_endl(unsigned int progress, unsigned int total) {
  out << " " << setw(8) << progress << " parts " << (progress * 100) / total << "%" << endl;
}

void cout_mb_endl(streampos progress, streampos total) {
  out << " " << setw(8) << fixed << float(progress) / 1000000 << " MB " << (progress * 100) / total << "%" << endl;
}

bool generate_map(settings_t &s, fs::path& world_path, fs::path& output_path) {
  out << endl << "Generating PNG Map" << endl << endl;
  
  out << "Threads: " << s.threads << std::endl;
  
  std::vector<player> players;
  std::vector<warp> warps;
  list<rotated_level_info> levels;
  
  bool any_db =
    s.show_players
    || s.show_signs
    || s.show_coordinates
    || s.show_warps;

  bool write_markers = 
    s.write_json || s.write_js;
  
  if (any_db) {
    out << " --- LOOKING FOR DATABASES --- " << endl;
    
    if (s.show_warps) {
      out << "warps: " << s.show_warps_path << ": " << flush;
      
      warps_db wdb(s.show_warps_path);
      
      try {
        wdb.read(warps);
        out << warps.size() << " warp(s) OK" << endl;
      } catch(warps_db_exception& e) {
        out << e.what() << endl;
      }
    }
    
    if (s.show_players) {
      fs::path show_players_path = world_path / "players";
      
      out << "players: " << show_players_path << ": " << flush;
      
      players_db pdb(show_players_path, s.show_players_set);
      
      try {
        pdb.read(players);
        out << players.size() << " player(s) OK" << endl;
      } catch(players_db_exception& e) {
        out << " " << e.what() << endl;
      }
    }

    if (s.show_signs) {
      out << "will look for signs in levels";
    }

    if (s.show_coordinates) {
      out << "will store shunk coordinates";
    }
  }
  
  {
    out << " --- SCANNING WORLD DIRECTORY --- " << endl;
    out << "world: " << world_path.string() << endl;
  }
  
  mc::world world(world_path);
  
  {
    nonstd::continious<unsigned int> reporter(100, cout_dot<unsigned int>, cout_uint_endl);
    mc::chunk_iterator iterator = world.get_iterator();
    
    while (iterator.has_next()) {
      reporter.add(1);
      
      boost::shared_ptr<mc::level_info> level;
      
      try {
        level = iterator.next();
      } catch(mc::bad_level& e) {
        out_log << e.where() << ": " << e.what() << std::endl;
        continue;
      }
      
      const mc::utils::level_coord coord = level->get_coord();
      
      uint64_t x2 = coord.get_x() * coord.get_x();
      uint64_t z2 = coord.get_z() * coord.get_z();
      uint64_t r2 = s.max_radius * s.max_radius;
      
      bool out_of_range = 
          coord.get_x() < s.min_x
          || coord.get_x() > s.max_x
          || coord.get_z() < s.min_z
          || coord.get_z() > s.max_z
          || x2 + z2 >= r2;
      
      if (out_of_range) {
        if (s.debug) {
          out_log << level->get_path() << ": position out of limit (" << coord.get_z() << "," << coord.get_z() << ")" << std::endl;
        }
        
        continue;
      }
      
      rotated_level_info rlevel =
        rotated_level_info(level, coord.rotate(s.rotation));
      
      levels.push_back(rlevel);
      world.update(rlevel.coord);
    }
    
    reporter.done(0);
    levels.sort();
  }

  if (levels.size() <= 0) {
    out << "No chunks to render" << endl;
    return 0;
  }
  
  if (s.debug) {
    out << " --- DEBUG WORLD INFO --- " << endl;
    out << "mc::world" << endl;
    out << "  min_x: " << world.min_x << endl;
    out << "  max_x: " << world.max_x << endl;
    out << "  min_z: " << world.min_z << endl;
    out << "  max_z: " << world.max_z << endl;
    out << "  levels: " << levels.size() << endl;
    out << "  chunk pos: " << world.chunk_x << "x" << world.chunk_y << endl;
  }
  
  boost::shared_ptr<engine_base> engine;
  
  switch (s.mode) {
    case Top: engine.reset(new topdown_engine(s, world)); break;
    case Oblique: engine.reset(new oblique_engine(s, world)); break;
    case ObliqueAngle: engine.reset(new obliqueangle_engine(s, world)); break;
    case Isometric: engine.reset(new isometric_engine(s, world)); break;
  }
  
  image_base::pos_t i_w, i_h;
  image_base::pos_t l_w, l_h;
  
  engine->get_boundaries(i_w, i_h);
  engine->get_level_boundaries(l_w, l_h);
  
  image_base::pos_t mem_x = i_w * i_h * 4 * sizeof(uint8_t);
  float memory_usage_mb = float(mem_x) / 1000000.0f; 
  float memory_limit_mb = float(s.memory_limit) / 1000000.0f;
  
  boost::shared_ptr<image_base> all;
  
  if (mem_x >= s.memory_limit) {
    {
      out << " --- BUILDING SWAP --- " << endl;
      out << "NOTE: A swap file is being built to accommodate high memory usage" << endl;
      out << "swap file: " << s.swap_file << endl;

      out << "swap size: " << memory_usage_mb << " MB" << endl;
      out << "memory limit: " << memory_limit_mb << endl;
    }
    
    cached_image* image;
    
    try {
      image = new cached_image(s.swap_file, i_w, i_h, l_w, l_h);
    } catch(std::ios::failure& e) {
      if (errno != 0) {
        error << s.swap_file << ": " << strerror(errno);
      } else {
        error << s.swap_file << ": " << e.what() << ": could not open file";
      }
      
      return false;
    }
    
    all.reset(image);
    
    nonstd::limited<streampos> c(1024 * 1024, cout_dot<streampos>, cout_mb_endl);
    
    try {
      image->build(c);
    } catch(std::ios::failure& e) {
      if (errno != 0) {
        error << s.swap_file << ": could not build cache: " << strerror(errno);
      } else {
        error << s.swap_file << ": could not build cache: " << e.what();
      }
      
      return false;
    }
  } else {
    {
      out << " --- ALLOCATING MEMORY --- " << endl;
      out << "memory usage: " << memory_usage_mb << " MB" << endl;
      out << "memory limit: " << memory_limit_mb << " MB" << endl;
    }
    
    all.reset(new memory_image(i_w, i_h));
  }
  
  unsigned int world_size = levels.size();
  
  renderer renderer(s, s.threads, world_size);
  
  std::vector<mc::marker> signs;
  std::list<rotated_level_info>::iterator lvlit = levels.begin();
  
  renderer.start();
  
  unsigned int queued = 0;

  unsigned int i;

  unsigned int prebuffer = s.threads * s.prebuffer;
  unsigned int filllimit = prebuffer / 2;
  
  nonstd::limited<unsigned int> c(50, cout_dot<unsigned int>, cout_uintpart_endl);
  c.set_limit(world_size);
  
  {
    out << " --- RENDERING --- " << endl;
  }
  
  int cache_hits = 0;
  
  for (i = 0; i < world_size; i++) {
    if (queued <= filllimit) {
      for (; queued < prebuffer && lvlit != levels.end(); lvlit++) {
        rotated_level_info rl = *lvlit;
        
        render_job job;
        
        boost::shared_ptr<mc::level> level(new mc::level(rl.level));
        
        job.engine = engine;
        job.level = level;
        
        try {
          level->read();
        } catch(mc::invalid_file& e) {
          out << level->get_path() << ": " << e.what() << endl;
          continue;
        }
         
        job.level = level;
        job.xPos = rl.coord.get_x();
        job.zPos = rl.coord.get_z();
        
        renderer.give(job);
        
        if (s.debug) { out << rl.level->get_path() << ": queued OK" << endl; }
        
        queued++;
      }
    }
    
    c.add(1);
    --queued;
    
    render_result p = renderer.get();

    if (p.fatal) {
      {
        out << p.level->get_path() << ": " << p.fatal_why << endl;
        continue;
      }
    }

    if (p.cache_hit) {
      ++cache_hits;
    }
    
    ///if (progress_c != NULL) progress_c(i, world_size);
    
    if (p.signs.size() > 0) {
      if (s.debug) { out << "Found " << p.signs.size() << " signs"; };
      signs.insert(signs.end(), p.signs.begin(), p.signs.end());
    }

    try {
      image_base::pos_t x, y;
      engine->w2pt(p.xPos, p.zPos, x, y);
      all->composite(x, y, p.operations);
    } catch(std::ios::failure& e) {
      out << s.swap_file << ": " << strerror(errno);
      return false;
    }
  }
  
  c.done(0);
  
  if (s.cache_use) {
    out << "cache_hits: " << cache_hits << "/" << world_size << endl;
  }
  
  //if (progress_c != NULL) progress_c(world_size, world_size);
  
  boost::ptr_vector<marker> markers;
  
  if (any_db) {
    text::font_face font(s.ttf_path, s.ttf_size, s.ttf_color);
    
    if (!write_markers) {
      try {
        font.init();
      } catch(text::text_error& e) {
        warnings.push_back(std::string("Failed to initialize font: ") + e.what());
      }
    }
    
    if (s.show_players) {
      text::font_face player_font = font;
      
      if (s.has_player_color) {
        player_font.set_color(s.player_color);
      }
      
      std::vector<player>::iterator plit = players.begin();
      
      /* initial code for projecting players */
      for (; plit != players.end(); plit++) { 
        player p = *plit;
        
        if (p.zPos / mc::MapZ < s.min_z) continue;
        if (p.zPos / mc::MapZ > s.max_z) continue;
        if (p.xPos / mc::MapX < s.min_x) continue;
        if (p.xPos / mc::MapX > s.max_x) continue;
        
        markers.push_back(new marker(p.name, "player", player_font, p.xPos, p.yPos, p.zPos));
      }
    }
    
    if (s.show_signs && signs.size() > 0) {
      text::font_face sign_font = font;
      
      if (s.has_sign_color) {
        sign_font.set_color(s.sign_color);
      }
      
      std::vector<mc::marker>::iterator lmit = signs.begin();
      
      for (; lmit != signs.end(); lmit++) {
        mc::marker lm = *lmit;
        
        if (!s.show_signs_filter.empty() && lm.text.find(s.show_signs_filter) == string::npos) {
          continue;
        }
        
        markers.push_back(new marker(lm.text, "sign", sign_font, lm.x, lm.y, lm.z));
      }
    }
    
    if (s.show_coordinates) {
      text::font_face coordinate_font = font;
      
      if (s.has_coordinate_color) {
        coordinate_font.set_color(s.coordinate_color);
      }
      
      for (lvlit = levels.begin(); lvlit != levels.end(); lvlit++) {
        rotated_level_info rl = *lvlit;
        mc::utils::level_coord c = rl.coord;
        boost::shared_ptr<mc::level_info> l = rl.level;
        
        if (c.get_z() - 4 < world.min_z) continue;
        if (c.get_z() + 4 > world.max_z) continue;
        if (c.get_x() - 4 < world.min_x) continue;
        if (c.get_x() + 4 > world.max_x) continue;
        if (c.get_z() % 10 != 0) continue;
        if (c.get_x() % 10 != 0) continue;
        std::stringstream ss;
        
        ss << "(" << l->get_x() * mc::MapX << ", " << l->get_z() * mc::MapZ << ")";
        markers.push_back(new marker(ss.str(), "coord", coordinate_font, c.get_x() * mc::MapX, 0, c.get_z() * mc::MapZ));
      }
    }
    
    if (s.show_warps) {
      text::font_face warp_font = font;
      
      if (s.has_warp_color) {
        warp_font.set_color(s.warp_color);
      }
      
      std::vector<warp>::iterator wit = warps.begin();
      
      /* initial code for projecting warps */
      for (; wit != warps.end(); wit++) { 
        warp w = *wit;
        
        if (w.zPos / mc::MapZ < s.min_z) continue;
        if (w.zPos / mc::MapZ > s.max_z) continue;
        if (w.xPos / mc::MapX < s.min_x) continue;
        if (w.xPos / mc::MapX > s.max_x) continue;
        
        marker *m = new marker(w.name, "warp", warp_font, w.xPos, w.yPos, w.zPos);
        markers.push_back(m);
      }
    }
  }
  
  engine_base::pos_t center_x, center_y;
  engine->wp2pt(0, 0, 0, center_x, center_y);
  
  if (write_markers) {
    if (!any_db) {
      hints.push_back("Use `--write-json' in combination with `--show-*' in order to write different types of markers to file");
    }
    
    json::object file;
    json::object* json_static = new json::object;
    
    json_static->put("MapX", new json::number(mc::MapX));
    json_static->put("MapY", new json::number(mc::MapY));
    json_static->put("MapZ", new json::number(mc::MapZ));
    
    file.put("st", json_static);
    
    json::object* json_world = new json::object;
    
    json_world->put("cx", new json::number(center_x));
    json_world->put("cy", new json::number(center_y));
    json_world->put("dx", new json::number((world.diff_x + 1) * mc::MapX));
    json_world->put("dz", new json::number((world.diff_z + 1) * mc::MapZ));
    json_world->put("dy", new json::number(mc::MapY));
    json_world->put("mn_x", new json::number(world.min_x * 16));
    json_world->put("mn_z", new json::number(world.min_z * 16));
    json_world->put("mx_x", new json::number(world.max_x * 16));
    json_world->put("mx_z", new json::number(world.max_z * 16));
    json_world->put("mode", new json::number(s.mode));
    
    file.put("world", json_world);
    
    json::array* markers_array = new json::array;
    populate_markers(s, markers_array, engine, markers);
    file.put("markers", markers_array);
    
    if (s.write_json) {
      out << "Writing json information: " << s.write_json_path.string() << endl;
      std::ofstream of(s.write_json_path.string().c_str());
      of << file;
      of.close();
    }

    if (s.write_js) {
      out << "Writing js (javascript `var c10t_json') information: " << s.write_js_path.string() << endl;
      std::ofstream of(s.write_js_path.string().c_str());
      of << "var c10t_json = " << file << ";";
      of.close();
    }
  }
  else {
    overlay_markers(s, all, engine, markers);
  }
  
  if (s.use_split) {
    //boost::ptr_map<point2, image_base> parts;
    
    {
      out << " --- SAVING MULTIPLE IMAGES --- " << endl;
      out << "splitting on " << s.split << "px basis" << endl;
    }
    
    std::map<point2, image_base*> parts = image_split(all.get(), s.split);
    
    {
      out << "saving " << parts.size() << " images" << endl;
    }
    
    for (std::map<point2, image_base*>::iterator it = parts.begin(); it != parts.end(); it++) {
      const point2 p = it->first;
      boost::scoped_ptr<image_base> img(it->second);
      
      stringstream ss;
      ss << boost::format(output_path.string()) % p.x % p.y;
      
      std::string path = ss.str();
      
      png_format::opt_type opts;

      opts.center_x = center_x;
      opts.center_y = center_y;
      opts.comment = C10T_COMMENT;
      
      if (!img->save<png_format>(path, opts)) {
        out << path << ": Could not save image";
        continue;
      }
      
      out << path << ": OK" << endl;
    }
  }
  else {
    {
      out << " --- SAVING IMAGE --- " << endl;
      out << "path: " << output_path << endl;
    }
    
    png_format::opt_type opts;
    
    opts.center_x = center_x;
    opts.center_y = center_y;
    opts.comment = C10T_COMMENT;
    
    if (!all->save<png_format>(output_path.string(), opts)) {
      out << output_path << ": Could not save image";
      error << strerror(errno);
      return false;
    }
    
    out << output_path << ": OK" << endl;
  }
  
  return true;
}

bool generate_statistics(settings_t &s, fs::path& world_path, fs::path& output_path)
{
  out << endl << "Generating Statistics File" << endl << endl;
  
  std::vector<player> players;
  mc::world world(world_path);
  
  long statistics[mc::MaterialCount];

  for (int i = 0; i < mc::MaterialCount; i++) {
    statistics[i] = 0;
  }
    
  bool any_db =
    s.show_players
    || s.show_signs
    || s.show_coordinates
    || s.show_warps;
  
  if (any_db) {
    out << " --- LOOKING FOR DATABASES --- " << endl;
    
    if (s.show_players) {
      fs::path show_players_path = world_path / "players";
      
      out << "players: " << show_players_path << ": " << flush;
      
      players_db pdb(show_players_path, s.show_players_set);
      
      try {
        pdb.read(players);
        out << players.size() << " player(s) OK" << endl;
      } catch(players_db_exception& e) {
        out << " " << e.what() << endl;
      }
    }
  }
  
  {
    nonstd::continious<unsigned int> reporter(100, cout_dot<unsigned int>, cout_uint_endl);
    mc::chunk_iterator iterator = world.get_iterator();
    
    while (iterator.has_next()) {
      reporter.add(1);
        
      boost::shared_ptr<mc::level_info> level;
      
      try {
        level = iterator.next();
      } catch(mc::bad_level& e) {
        out_log << e.where() << ": " << e.what() << std::endl;
      }
      
      mc::utils::level_coord coord = level->get_coord();
      
      uint64_t x2 = coord.get_x() * coord.get_x();
      uint64_t z2 = coord.get_z() * coord.get_z();
      uint64_t r2 = s.max_radius * s.max_radius;
        
      bool out_of_range = 
            coord.get_x() < s.min_x
            || coord.get_x() > s.max_x
            || coord.get_z() < s.min_z
            || coord.get_z() > s.max_z
            || x2 + z2 >= r2;
        
      if (out_of_range) {
        if (s.debug) {
          out_log << level->get_path() << ": position out of limit (" << coord.get_z() << "," << coord.get_z() << ")" << std::endl;
        }
        continue;
      }
      
      mc::level level_data(level);
      
      try {
        level_data.read();
      } catch(mc::invalid_file& e) {
        out_log << level->get_path() << ": " << e.what();
        continue;
      }

      boost::shared_ptr<nbt::ByteArray> blocks = level_data.get_blocks();
      
      for (int i = 0; i < blocks->length; i++) {
        nbt::Byte block = blocks->values[i];
        statistics[block] += 1;
      }
      
      world.update(level->get_coord());
    }
    
    reporter.done(0);
  }

  ofstream stats(output_path.string().c_str());

  stats << "[WORLD]" << endl; 
  
  stats << "min_x " << world.min_x << endl;
  stats << "max_x " << world.max_x << endl;
  stats << "min_z " << world.min_z << endl;
  stats << "max_z " << world.max_z << endl;

  if (s.show_players) {
    stats << "[PLAYERS]" << endl;
    
    std::vector<player>::iterator plit = players.begin();
    
    for (; plit != players.end(); plit++) { 
      player p = *plit;
      stats << p.name << " " << p.xPos << " " << p.yPos << " " << p.zPos << endl;
    }
  }
  
  stats << "[BLOCKS]" << endl; 
  
  for (int i = 0; i < mc::MaterialCount; i++) {
    stats << setw(3) << i << " " << setw(24) << mc::MaterialName[i] << " " << statistics[i] << endl;
  }
  
  stats.close();
  
  if (stats.fail()) {
    error << "failed to write statistics to " << output_path;
    return false;
  }

  out << "statistics written to " << output_path;
  return true;
}

int do_help() {
  out << "This program was made possible because of the work and inspiration by ZomBuster and Firemark" << endl;
  out << "" << endl;
  out << "Written by Udoprog et al." << endl;
  out << "" << endl;
  out << "The following libraries are in use for this program:" << endl
       << "  zlib (compression)"                  << endl
       << "    http://www.zlib.net"               << endl
       << "  boost (thread, filesystem)"          << endl
       << "    http://www.boost.org"              << endl
       << "  libpng (portable network graphics)"  << endl
       << "    http://www.libpng.org"             << endl
       << "  libfreetype (font loading)"          << endl
       << "    http://www.freetype.org"           << endl
       << "" << endl;
# if defined(C10T_DISABLE_THREADS)
  out << endl;
  out << "C10T_DISABLE_THREADS: Threads has been disabled for this build" << endl;
# endif
  out << endl;
  out << "Usage: c10t [options]" << endl;
  out << "Options:" << endl
       /*******************************************************************************/
    << "  -w, --world <world>       - use this world directory as input                " << endl
    << "  -o, --output <output>     - use this file as output file for generated png   " << endl
    << "  -S, --statistics <output> - create a statistics file of the entire world     " << endl
    << endl
    << "  --log [file]              - Specify another location for logging warnings,   " << endl
    << "                              defaults to `c10t.log'                           " << endl
    << "  --no-log                  - Suppress logging of warnings                     " << endl
    << endl
    << "  -s, --silent              - execute silently, printing nothing except errors " << endl
    << "  -h, --help                - display this help text                           " << endl
    << "  -v, --version             - display version information                      " << endl
    << "  -D, --debug               - display debug information while executing        " << endl
    << "  -l, --list-colors         - list all available colors and block types        " << endl
    << endl
    << "Rendering options:" << endl
    << "  -q, --oblique             - Oblique rendering                                " << endl
    << "  -y, --oblique-angle       - Oblique angle rendering                          " << endl
    << "  -z, --isometric           - Isometric rendering                              " << endl
    << "  -r <degrees>              - rotate the rendering 90, 180 or 270 degrees CW   " << endl
    << endl
    << "  -n, --night               - Night-time rendering mode                        " << endl
    << "  -H, --heightmap           - Heightmap rendering mode (black to white)        " << endl
    << "      --disable-skylight    - Disables skylight (faster rendering)             " << endl
    << endl
    << "Filtering options:" << endl
    << "  -e, --exclude <blockid>   - Exclude block-id from render (multiple occurences" << endl
    << "                              is possible)                                     " << endl
    << "  -i, --include <blockid>   - Include only this block-id in render (multiple   " << endl
    << "                              occurences is possible)                          " << endl
    << "  -a, --hide-all            - Show no blocks except those specified with '-i'  " << endl
    << "  -c, --cave-mode           - Cave mode - top down until solid block found,    " << endl
    << "                              then render bottom outlines only                 " << endl
    << "      --hell-mode           - Hell mode - top down until non-solid block found," << endl
    << "                              then render normally (a.k.a. nether)             " << endl
    << endl
    << "  -t, --top <int>           - Splice from the top, must be less than 128       " << endl
    << "  -b, --bottom <int>        - Splice from the bottom, must be greater than or  " << endl
    << "                              equal to zero.                                   " << endl
    << "  -L, --limits <int-list>   - Limit render to certain area. int-list form:     " << endl
    << "                              North,South,East,West, e.g.                      " << endl
    << "                              -L 0,100,-10,20 limiting between 0 and 100 in the" << endl
    << "                              north-south direction and between -10 and 20 in  " << endl
    << "                              the east-west direction.                         " << endl
    << "                              Note: South and West are the positive directions." << endl
    << "  -R, --radius <int>        - Limit render to a specific radius, useful when   " << endl
    << "                              your map is absurdly large and you want a 'fast' " << endl
    << "                              limiting option.                                 " << endl
    << endl
    << "  -N, --no-check            - Ignore missing <world>/level.dat                 " << endl
       /*******************************************************************************/
    << endl
    << "Performance options:" << endl
    << "  -M, --memory-limit <MB>   - Will limit the memory usage caching operations to" << endl
    << "                              file when necessary                              " << endl
    << "  -C, --swap-file <file>    - Swap file to use when memory limit `-M' is       " << endl
    << "                              reached, defaults to `swap.bin'                  " << endl
    << "  -m, --threads <int>       - Specify the amount of threads to use, for maximum" << endl
    << "                              efficency, this should match the amount of cores " << endl
    << "                              on your machine                                  " << endl
    << "      --prebuffer <int>     - Specify how many jobs to prebuffer for each      " << endl
    << "                              individual thread                                " << endl
    << "                                                                               " << endl
    << "  -B <set>                  - Specify the base color for a specific block id   " << endl
    << "                              <set> has the format <blockid>=<color>           " << endl
    << "                              <8 digit hex> specifies the RGBA values as       " << endl
    << "                              `<int>,<int>,<int>[,<int>]'. The side color will " << endl
    << "                              be a darkened variant of the base                " << endl
    << "                              example: `-B Grass=0,255,0,120'                  " << endl
    << "                              NOTE: Use only for experimentation, for a more   " << endl
    << "                                    permanent solution, use color palette files" << endl
    // this has been commented out since it is planned to be integrated for '-B' as a token scanning
    /*<< "  --side <set>              - Specify the side color for a specific block id   " << endl
    << "                              this uses the same format as '-B' only the color " << endl
    << "                              is applied to the side of the block              " << endl*/
    << "  -p, --split <px>          - Split the render into parts which must be <px>   " << endl
    << "                              pixels squared. `output' name must contain two   " << endl
    << "                              format specifiers `%d' for x and y position.     " << endl
       /*******************************************************************************/
    << endl
    << "Other Options:" << endl
    << "  -x, --binary              - Will output progress information in binary form, " << endl
    << "                              good for integration with third party tools      " << endl
    << "  --require-all             - Will force c10t to require all chunks or fail    " << endl
    << "                              not ignoring bad chunks                          " << endl
    << "  --show-players[=NICKLIST] - Will draw out player position and names from the " << endl
    << "                              players database in <world>/players              " << endl
    << "                              it is possible to define which nicks to show by  " << endl
    << "                              specifying a comma separated list of nicks       " << endl
    << "  --show-signs[=PREFIX]     - Will draw out signs from all chunks, if PREFIX   " << endl
    << "                              is specified, only signs matching the prefix will" << endl
    << "                              be drawn                                         " << endl
    << "  --show-warps=<file>       - Will draw out warp positions from the specified  " << endl
    << "                              warps.txt file, as used by hey0's mod            " << endl
    << "  --show-coordinates        - Will draw out each chunks expected coordinates   " << endl
    << "  -P <file>                 - use <file> as palette, each line should take the " << endl
    << "                              form: <block-id> ' ' <color> ' ' <color>         " << endl
    << "  -W <file>                 - write the default color palette to <file>, this  " << endl
    << "                              is useful for figuring out how to write your own " << endl
    << "  --pedantic-broad-phase    - Will enforce that all level chunks are parsable  " << endl
    << "                              during broad phase by getting x/y/z positions    " << endl
    << "                              from a quick parsing                             " << endl
    << "  --no-alpha                - Set all colors alpha channel to opaque (solid)   " << endl
    << "  --striped-terrain         - Darken every other block on a vertical basis     " << endl
    << "                              which helps to distinguish heights               " << endl
    << "  --write-json <file>       - Write markers to <file> in JSON format instead of" << endl
    << "                              printing them on map                             " << endl
    << "  --write-js <file>         - Same as `write-json' with the exception that the " << endl
    << "                              result will be a valid javascript file containing" << endl
    << "                              a declaration for `var c10t_json'                " << endl
       /*******************************************************************************/
    << endl
    << "Font Options:" << endl
    << "  --ttf-path <font>         - Use the following ttf file when drawing text.    " << endl
    << "                              defaults to `font.ttf'                           " << endl
    << "  --ttf-size <size>         - Use the specified font size when drawing text.   " << endl
    << "                              defaults to `12'                                 " << endl
    << "  --ttf-color <color>       - Use the specified color when drawing text.       " << endl
    << "                              defaults to `0,0,0,255' (black)                  " << endl
    << "  --sign-color <color>      - Use the specified color when drawing signs.      " << endl
    << "                              defaults to <ttf-color>                          " << endl
    << "  --player-color <color>    - Use the specified color when showing players.    " << endl
    << "                              defaults to <ttf-color>                          " << endl
    << "  --warp-color <color>      - Use the specified color when showing warps.      " << endl
    << "                              defaults to <ttf-color>                          " << endl
    << "  --coordinate-color <color>                                                   " << endl
    << "                            - Use the specified color when drawing coordinates." << endl
    << "                              defaults to <ttf-color>                          " << endl
    << "  --cache-key <key>         - Indicates that c10t should cache operations using" << endl
    << "                              the unique cache key <key>, this should represent" << endl
    << "                              an unique combination of options. The cache files" << endl
    << "                              will be put in                                   " << endl
    << "                              <cache-dir>/<cache-key>/c.<coord>.cmap           " << endl
    << "  --cache-dir <dir>         - Use the following directory as cache directory   " << endl
    << "                              defaults to 'cache' if not specified             " << endl
    << "  --cache-compress          - Compress the cache files using zlib compression  " << endl
       /*******************************************************************************/
    << endl;
  out << endl;
  out << "Typical usage:" << endl;
  out << "    c10t -w /path/to/world -o /path/to/png.png" << endl;
  out << endl;
  out << "  Utilize render cache and apply a 256 MB memory restriction (rest will be written to image.dat):" << endl;
  out << "    c10t -w /path/to/world -o /path/to/png.png --cache-key='compressed' --cache-compress -M 256 -C image.dat" << endl;
  out << endl;
  out << "  Print out player positions using the font `example.ttf'" << endl;
  out << "    c10t -w /path/to/world -o /path/to/png.png --show-players --ttf-font example.ttf" << endl;
  out << endl;
  out << "  Split the result into multiple files, using 10 chunks across in each file, the two number formatters will be replaced with the x/z positions of the chunks" << endl;
  out << "    c10t -w /path/to/world -o /path/to/png.%d.%d.png --split 10" << endl;
  out << endl;
  return 0;
}

int do_version() {
  out << "c10t - a cartography tool for minecraft" << endl;
# if defined(C10T_DISABLE_THREADS)
  out << endl;
  out << "C10T_DISABLE_THREADS: Threads have been disabled for this build" << endl;
  out << endl;
# endif
  out << "version: " << C10T_VERSION << ", built on " << __DATE__ << endl;
  out << "by: " << C10T_CONTACT << endl;
  out << "site: " << C10T_SITE << endl;
  return 0;
}

int do_colors() {
  out << "List of material Colors (total: " << mc::MaterialCount << ")" << endl;
  
  for (int i = 0; i < mc::MaterialCount; i++) {
    out << i << ": " << mc::MaterialName[i] << " = " << mc::MaterialColor[i] << endl;
  }
  
  return 0;
}

int main(int argc, char *argv[]){
  nullstream nil;
  
  out.precision(2);
  out.setf(ios_base::fixed);
  
  mc::initialize_constants();

  settings_t s;
  
  if (!read_opts(s, argc, argv)) {
    goto exit_error;
  }

  switch(s.action) {
    case Version:
      return do_version();
    case Help:
      return do_help();
    case ListColors:
      return do_colors();
    case None:
      error << "No action specified, please type `c10t -h' for help";
      goto exit_error;
    default: break;
  }

  if (s.binary) out.rdbuf(out_log.rdbuf());
  if (s.silent) out.rdbuf(nil.rdbuf());
  if (!s.no_log) out_log.open(s.output_log.string().c_str());
  
  if (s.memory_limit_default) {
    hints.push_back("To use less memory, specify a memory limit with `-M <MB>', if it is reached c10t will swap to disk instead");
  }
  
  if (s.cache_use) {
    if (!fs::is_directory(s.cache_dir)) {
      error << "Directory required for caching: " << s.cache_dir.string();
      goto exit_error;
    }
    
    // then create the subdirectory using cache_key
    s.cache_dir = s.cache_dir / s.cache_key;
    
    if (!fs::is_directory(s.cache_dir)) {
      out << "Creating directory for caching: " << s.cache_dir.string() << endl;
      fs::create_directory(s.cache_dir);
    }
    
    {
      out << "Caching to directory: " << s.cache_dir << std::endl;
      out << "Cache compression: " << (s.cache_compress ? "ON" : "OFF")  << std::endl;
    }
  }
  
  if (!s.palette_write_path.empty()) {
    if (!do_write_palette(s, s.palette_write_path)) {
      goto exit_error;
    }

    out << "Sucessfully wrote palette to " << s.palette_write_path << endl;
  }
  
  if (!s.palette_read_path.empty()) {
    if (!do_read_palette(s, s.palette_read_path)) {
      goto exit_error;
    }

    out << "Sucessfully read palette from " << s.palette_read_path << endl;
  }
  
  if (s.world_path.empty())
  {
    error << "You must specify a world to render using `-w <directory>'";
    goto exit_error;
  }
  
  if (!s.nocheck)
  {
    fs::path level_dat = s.world_path / "level.dat";
    
    if (!fs::exists(level_dat)) {
      error << "Does not exist: " << level_dat.string();
      goto exit_error;
    }
  }
  
  switch(s.action) {
    case GenerateWorld:
      /* do some nice sanity checking prior to generating since this might
       * catch a couple of errors */

      if (s.output_path.empty()) {
        error << "You must specify output file using `-o <file>'";
        goto exit_error;
      }
      
      if (!fs::is_directory(s.output_path.parent_path())) {
        error << "Output directory does not exist: " << s.output_path;
        goto exit_error;
      }
      
      if (s.use_split) {
        try {
          boost::format(fs::basename(s.output_path)) % 0 % 0;
        } catch (boost::io::too_many_args& e) {
          error << "The `-o' parameter must contain two number format specifiers `%d' (x and y coordinates) - example: -o out/base.%d.%d.png";
          goto exit_error;
        }
      }
  
      if (!generate_map(s, s.world_path, s.output_path)) goto exit_error;
      break;
    case GenerateStatistics:
      if (!generate_statistics(s, s.world_path, s.statistics_path)) goto exit_error;
      break;
    default:
      error << "No action specified";
      goto exit_error;
  }
  
  if (hints.size() > 0 || warnings.size() > 0) {
    int i = 1;
    
    for (vector<std::string>::iterator it = warnings.begin(); it != warnings.end(); it++) {
      out << "WARNING " << i++ << ": " << *it << endl;
    }
    
    i = 1;
    for (vector<std::string>::iterator it = hints.begin(); it != hints.end(); it++) {
      out << "Hint " << i++ << ": " << *it << endl;
    }

    out << endl;
  }
  
  if (s.binary) {
    cout_end();
  }
  else {
    out << argv[0] << ": all done!" << endl;
  }
  
  mc::deinitialize_constants();
  
  if (!s.no_log) {
    out << "Log written to " << s.output_log << endl;
    out_log.close();
  }
  
  return 0;
exit_error:
  if (s.binary) {
    cout_error(error.str());
  }
  else {
    out << argv[0] << ": " << error.str() << endl;
  }
  
  mc::deinitialize_constants();
  
  if (!s.no_log) {
    out << "Log written to " << s.output_log << endl;
    out_log.close();
  }
  
  return 1;
}
