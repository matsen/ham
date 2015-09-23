#ifndef HAM_TRACK_H
#define HAM_TRACK_H

#include <map>
#include <cassert>
#include <stdexcept>

#include "text.h"
#include "mathutils.h"

using namespace std;
namespace ham {

class Tracks;
// ----------------------------------------------------------------------------------------
class Track {
  friend class Tracks;
public:
  Track() {}
  Track(string, vector<string>);
  void set_name(string nm) { name_ = nm; }
  void AddSymbol(string symbol);
  void AddSymbols(vector<string> &symbols);

  string name() { return name_; }
  size_t alphabet_size() { return alphabet_.size(); }
  string symbol(size_t iter) { return alphabet_.at(iter); }  // return <iter>th element of <alphabet_> (which is probably a letter, but could be several letters or something else). NOTE throws std:out_of_range exception if <iter> is invalide
  uint8_t symbol_index(const string &symbol);
  string Stringify();
private:
  string name_;
  vector<string> alphabet_;  // vector of this track's allowed symbols (eg {A,C,G,T})
  map<string, uint8_t> symbol_indices_;
};

// ----------------------------------------------------------------------------------------
class Tracks {
public:
  void push_back(Track*);
  size_t size() { return tracks_.size(); }
  Track *track(const string &name) {
    if(!indices_.count(name)) {
      cerr << "ERROR: track '" << name << "' not found!" << endl;
      throw runtime_error("configuration");
    }
    return tracks_[indices_[name]];
  }
  Track *operator[](size_t i) { return tracks_.at(i); }
private:
  vector<Track*> tracks_;
  map<string, size_t> indices_;
};

}
#endif
