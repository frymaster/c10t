// Distributed under the BSD License, see accompanying LICENSE.txt
// (C) Copyright 2010 John-John Tedro et al.
#ifndef _COLOR_H_
#define _COLOR_H_

#include <boost/lexical_cast.hpp>
#include <stdint.h>
#include <assert.h>

#include <string>
#include <sstream>
#include <ostream>

struct color{
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
  
  color(color *c) : r(c->r), g(c->g), b(c->b), a(c->a) { }
  
  color() : r(0xff), g(0xff), b(0xff), a(0x00) { }
  
  color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) :
    r(r), g(g), b(b), a(a) {
  }
  
  color(uint32_t rgba) :
    r(r >> 24), g((g >> 16) & 0xff), b((b >> 8) & 0xff), a(a & 0xff) {
  }
  
  bool is_opaque() const {
    return a == 0xff;
  }
  
  bool is_transparent() const {
    return a == 0x00;
  }
  
  ~color(){
  }
  
  void overlay(const color &other);
  void underlay(const color &other);
  void darken(uint8_t c);
  void blend(const color &other);
    
  friend std::ostream& operator<<(std::ostream& out, const color& c) // output
  {
      out << "color(" << (int)c.r << ", " << (int)c.g << ", " << (int)c.b << ", " << (int)c.a << ")";
      return out;
  }
};

#endif /* _COLOR_H_ */