#include "jobholder.h"

// ----------------------------------------------------------------------------------------
KBounds KBounds::LogicalOr(KBounds rhs) {
  KBounds kbr(rhs); // return value
  if(vmin < kbr.vmin) kbr.vmin = vmin;
  if(dmin < kbr.dmin) kbr.dmin = dmin;
  if(vmax > kbr.vmax) kbr.vmax = vmax;
  if(dmax > kbr.dmax) kbr.dmax = dmax;
  return kbr;
}

// ----------------------------------------------------------------------------------------
void Result::check_boundaries(KSet best, KBounds kbounds) {
  // if(kbounds.vmax - kbounds.vmin <= 1 || kbounds.dmax - kbounds.dmin <= 2) return; // if k space is very narrow, we expect the max to be on the boundary, so ignore boundary errors

  int delta(2);  // be VERY VERY CAREFUL about subtracting move than the value off of a size_t. Yes, I know I should follow the google standards and not use unsigned integers but it wasn't my choice at the start... I'll switch eventually

  // see if we need to expand
  if(best.v == kbounds.vmin) {
    boundary_error_ = true;
    better_kbounds_.vmin = max((int)1, (int)kbounds.vmin - delta);
  }
  if(best.v == kbounds.vmax - 1) {
    boundary_error_ = true;
    better_kbounds_.vmax = kbounds.vmax + delta;
  }
  if(best.d == kbounds.dmin) {
    boundary_error_ = true;
    better_kbounds_.dmin = max((int)1, (int)kbounds.dmin - delta);
  }
  if(best.d == kbounds.dmax - 1) {
    boundary_error_ = true;
    better_kbounds_.dmax = kbounds.dmax + delta;
  }

  if(boundary_error_ && better_kbounds_.equals(kbounds))
    could_not_expand_ = true;
}

// ----------------------------------------------------------------------------------------
void HMMHolder::CacheAll() {
  for(auto & region : gl_.regions_) {
    for(auto & gene : gl_.names_[region]) {
      string infname(hmm_dir_ + "/" + gl_.SanitizeName(gene) + ".yaml");
      if(ifstream(infname)) {
        cout << "    read " << infname << endl;
        hmms_[gene] = new Model;
        hmms_[gene]->Parse(infname);
      }
    }
  }
}

// ----------------------------------------------------------------------------------------
Model *HMMHolder::Get(string gene, bool debug) {
  if(hmms_.find(gene) == hmms_.end()) {   // if we don't already have it, read it from disk
    hmms_[gene] = new Model;
    string infname(hmm_dir_ + "/" + gl_.SanitizeName(gene) + ".yaml");
    // if (true) cout << "    read " << infname << endl;
    hmms_[gene]->Parse(infname);
  }
  return hmms_[gene];
}

// ----------------------------------------------------------------------------------------
HMMHolder::~HMMHolder() {
  for(auto & entry : hmms_)
    delete entry.second;
}

// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
JobHolder::JobHolder(GermLines &gl, HMMHolder &hmms, string algorithm, vector<string> only_genes):
  gl_(gl),
  hmms_(hmms),
  algorithm_(algorithm),
  debug_(0),
  chunk_cache_(false),
  // total_score_(-INFINITY),
  n_best_events_(5) {
  if(only_genes.size() > 0) {
    for(auto & region : gl_.regions_)  // initialize
      only_genes_[region] = set<string>();
    for(auto & gene : only_genes)  // insert each gene in the proper region
      only_genes_[gl_.GetRegion(gene)].insert(gene);
    for(auto & region : gl_.regions_) // then make sure we have at least one gene for each region
      if(only_genes_[region].size() == 0)
        throw runtime_error("ERROR jobholder didn't get any genes for " + region + " region");
  }
}

// ----------------------------------------------------------------------------------------
JobHolder::~JobHolder() {
  Clear();
}

// ----------------------------------------------------------------------------------------
void JobHolder::Clear() {
  for(auto & gene_map : trellisi_) {
    string gene(gene_map.first);
    for(auto & query_str_map : gene_map.second) {
      delete query_str_map.second;
      if(paths_[gene][query_str_map.first])   // set to nullptr if no valid path
        delete paths_[gene][query_str_map.first];
    }
  }
  trellisi_.clear();
  paths_.clear();
  all_scores_.clear();
  best_per_gene_scores_.clear();
}

// ----------------------------------------------------------------------------------------
Sequences JobHolder::GetSubSeqs(Sequences &seqs, KSet kset, string region) {
  // get subsequences for one region
  size_t k_v(kset.v), k_d(kset.d);
  if(region == "v")
    return Sequences(seqs, 0, k_v);  // v region (plus vd insert) runs from zero up to k_v
  else if(region == "d")
    return Sequences(seqs, k_v, k_d);  // d region (plus dj insert) runs from k_v up to k_v + k_d
  else if(region == "j")
    return Sequences(seqs, k_v + k_d, seqs.GetSequenceLength() - k_v - k_d);  // j region runs from k_v + k_d to end
  else
    assert(0);
}

// ----------------------------------------------------------------------------------------
map<string, Sequences> JobHolder::GetSubSeqs(Sequences &seqs, KSet kset) {
  // get subsequences for all regions
  map<string, Sequences> subseqs;
  for(auto & region : gl_.regions_)
    subseqs[region] = GetSubSeqs(seqs, kset, region);
  return subseqs;
}

// ----------------------------------------------------------------------------------------
Result JobHolder::Run(Sequence seq, KBounds kbounds) {
  Sequences seqs;
  seqs.AddSeq(seq);
  return Run(seqs, kbounds);
}

// ----------------------------------------------------------------------------------------
Result JobHolder::Run(Sequences seqs, KBounds kbounds) {
  assert(kbounds.vmax > kbounds.vmin && kbounds.dmax > kbounds.dmin); // make sure max values for k_v and k_d are greater than their min values
  assert(kbounds.vmin > 0 && kbounds.dmin > 0);  // you get the loveliest little seg fault if you accidentally pass in zero for a lower bound
  Clear();
  assert(trellisi_.size() == 0 && paths_.size() == 0 && all_scores_.size() == 0);
  map<KSet, double> best_scores; // best score for each kset (summed over regions)
  map<KSet, double> total_scores; // total score for each kset (summed over regions)
  map<KSet, map<string, string> > best_genes; // map from a kset to its corresponding triplet of best genes

  Result result(kbounds);

  // loop over k_v k_d space
  double best_score(-INFINITY);
  KSet best_kset(0, 0);
  double *total_score = &result.total_score_;  // total score for all ksets
  int n_too_long(0);
  for(size_t k_v = kbounds.vmax - 1; k_v >= kbounds.vmin; --k_v) {
    for(size_t k_d = kbounds.dmax - 1; k_d >= kbounds.dmin; --k_d) {
      if(k_v + k_d >= seqs.GetSequenceLength()) {
        ++n_too_long;
        continue;
      }
      KSet kset(k_v, k_d);
      RunKSet(seqs, kset, &best_scores, &total_scores, &best_genes);
      *total_score = AddInLogSpace(total_scores[kset], *total_score);  // sum up the probabilities for each kset, log P_tot = log \sum_i P_k_i
      if(debug_ == 2 && algorithm_ == "forward") printf("            %9.2f (%.1e)  tot: %7.2f\n", total_scores[kset], exp(total_scores[kset]), *total_score);
      if(best_scores[kset] > best_score) {
        best_score = best_scores[kset];
        best_kset = kset;
      }
      if(algorithm_ == "viterbi" && best_scores[kset] != -INFINITY)
        PushBackRecoEvent(seqs, kset, best_genes[kset], best_scores[kset], &result.events_);
    }
  }
  if(debug_ && n_too_long > 0) cout << "      skipped " << n_too_long << " k sets 'cause they were longer than the sequence" << endl;

  // return if no valid path
  if(best_kset.v == 0) {
    cout << "ERROR no valid paths for " << seqs.name_str() << endl;
    result.no_path_ = true;
    return result;
  }

  // sort vector of events by score, stream info to stderr, and print the top n_best_events_
  if(algorithm_ == "viterbi") {
    sort(result.events_.begin(), result.events_.end());
    reverse(result.events_.begin(), result.events_.end());
    if(debug_ == 2) {
      assert(n_best_events_ <= result.events_.size());
      // for(size_t ievt = 0; ievt < n_best_events_; ++ievt) {
      //   result.events_[ievt].Print(gl_, 0, 0, false, false, "          ");  // man, I wish I had keyword args
      //   if(seqs.n_seqs() == 2)
      //     result.events_[ievt].Print(gl_, 0, 0, true, true, "          ");
      // }
    }
  }
  // StreamOutput(total_score_);  // NOTE this must happen after sorting in viterbi

  // print debug info
  if(debug_) {
    if(algorithm_ == "viterbi") {
      cout << "    " << setw(48) << seqs.name_str();
      cout << "   " << kbounds.vmin << "-" << kbounds.vmax - 1 << "   " << kbounds.dmin << "-" << kbounds.dmax - 1; // exclusive...
      cout << "    best kset: " << setw(4) << best_kset.v << setw(4) << best_kset.d << setw(12) << best_score << endl;
    } else {
      printf("        %9.3f", *total_score);
      cout << "   " << kbounds.vmin << "-" << kbounds.vmax - 1 << "   " << kbounds.dmin << "-" << kbounds.dmax - 1; // exclusive...
      cout << "    " << seqs.name_str() << endl;
    }
  }

  result.check_boundaries(best_kset, kbounds);
  if(debug_ && result.boundary_error()) {   // not necessarily a big deal yet -- the bounds get automatical expanded
    cout << "      WARNING max at boundary for " << seqs.name_str()
         << "  k_v: " << best_kset.v << "(" << kbounds.vmin << "-" << kbounds.vmax - 1 << ")"
         << "  k_d: " << best_kset.d << "(" << kbounds.dmin << "-" << kbounds.dmax - 1 << ")"
         << "    better: " << result.better_kbounds().stringify() << endl;
    if(result.could_not_expand())
      cout << "      WARNING couldn't expand though!" << endl;
  }

  return result;
}

// ----------------------------------------------------------------------------------------
void JobHolder::FillTrellis(Sequences query_seqs, vector<string> query_strs, string gene, double *score, string &origin) {
  *score = -INFINITY;
  // initialize trellis and path
  if(trellisi_.find(gene) == trellisi_.end()) {
    trellisi_[gene] = map<vector<string>, trellis*>();
    paths_[gene] = map<vector<string>, TracebackPath*>();
  }
  origin = "scratch";
  if(chunk_cache_) {   // figure out if we've already got a trellis with a dp table which includes the one we're about to calculate (we should, unless this is the first kset)
    for(auto & query_str_map : trellisi_[gene]) {
      vector<string> tmp_query_strs(query_str_map.first);
      // assert(0);  // fix the below!

      // loop over all the query strings for this trellis to see if they all match
      bool found_match(true);
      for(size_t iseq = 0; iseq < tmp_query_strs.size(); ++iseq) {  // NOTE this starts to seem like it might be bottlenecking me when I'm applying it for short d sequences
        if(tmp_query_strs[iseq].find(query_strs[iseq]) != 0) {
          found_match = false;
          break;
        }
      }


      // if they all match, then use it
      if(found_match) {
        for(size_t iseq = 0; iseq < tmp_query_strs.size(); ++iseq)
          assert(tmp_query_strs[iseq].find(query_strs[iseq]) == 0);
        trellisi_[gene][query_strs] = new trellis(hmms_.Get(gene, debug_), query_seqs, trellisi_[gene][tmp_query_strs]);
        origin = "chunk";
        break;
      }
    }
  }

  if(!trellisi_[gene][query_strs])   // if didn't find a suitable chunk cached trellis
    trellisi_[gene][query_strs] = new trellis(hmms_.Get(gene, debug_), query_seqs);
  trellis *trell(trellisi_[gene][query_strs]); // this pointer's just to keep the name short

  if(algorithm_ == "viterbi") {
    trell->Viterbi();
    *score = trell->ending_viterbi_log_prob();  // NOTE still need to add the gene choice prob to this score (it's done in RunKSet)
    if(trell->ending_viterbi_log_prob() == -INFINITY) {   // no valid path through hmm
      paths_[gene][query_strs] = nullptr;
      if(debug_ == 2) cout << "                    arg " << gene << " " << *score << " " << origin << endl;
    } else {
      paths_[gene][query_strs] = new TracebackPath(hmms_.Get(gene, debug_));
      trell->Traceback(*paths_[gene][query_strs]);
      assert(trell->ending_viterbi_log_prob() == paths_[gene][query_strs]->score());  // NOTE it would be better to just not store the darn score in both the places to start with, rather than worry here about them being the same
      // if(debug_ == 2) PrintPath(query_strs, gene, *score, origin);
    }
    assert(fabs(*score) > 1e-200);
    assert(*score == -INFINITY || paths_[gene][query_strs]->size() > 0);
  } else if(algorithm_ == "forward") {
    trell->Forward();
    paths_[gene][query_strs] = nullptr;  // avoids violating the assumption that paths_ and trellisi_ have the same entries
    *score = trell->ending_forward_log_prob();  // NOTE still need to add the gene choice prob to this score
  } else {
    assert(0);
  }
}

// ----------------------------------------------------------------------------------------
void JobHolder::PrintPath(vector<string> query_strs, string gene, double score, string extra_str) {  // NOTE query_str is seq1xseq2 for pair hmm
  if(score == -INFINITY) {
    // cout << "                    " << gene << " " << score << endl;
    return;
  }
  vector<string> path_names = paths_[gene][query_strs]->name_vector();
  if(path_names.size() == 0) {
    if(debug_) cout << "                     " << gene << " has no valid path" << endl;
    return;
  }
  assert(path_names.size() > 0);  // this will happen if the ending viterbi prob is 0, i.e. if there's no valid path through the hmm (probably the sequence or hmm lengths are screwed up)
  assert(path_names.size() == query_strs[0].size());
  size_t left_insert_length = GetInsertLength("left", path_names);
  size_t right_insert_length = GetInsertLength("right", path_names);
  size_t left_erosion_length = GetErosionLength("left", path_names, gene);
  size_t right_erosion_length = GetErosionLength("right", path_names, gene);

  string germline(gl_.seqs_[gene]);
  string modified_seq = germline.substr(left_erosion_length, germline.size() - right_erosion_length - left_erosion_length);
  for(size_t i = 0; i < left_insert_length; ++i)
    modified_seq = "i" + modified_seq;
  for(size_t i = 0; i < right_insert_length; ++i)
    modified_seq = modified_seq + "i";
  assert(modified_seq.size() == query_strs[0].size());
  assert(germline.size() + left_insert_length - left_erosion_length - right_erosion_length + right_insert_length == query_strs[0].size());
  TermColors tc;
  cout
      << "                    "
      << (left_erosion_length > 0 ? ".." : "  ") << tc.ColorMutants("red", modified_seq, "", query_strs) << (right_erosion_length > 0 ? ".." : "  ")
      << "  " << extra_str
      // NOTE this doesn't include the overall gene prob!
      // << setw(12) << paths_[gene][query_strs]->score()
      << setw(12) << score
      << setw(25) << gene
      << endl;
}

// ----------------------------------------------------------------------------------------
void JobHolder::PushBackRecoEvent(Sequences &seqs, KSet kset, map<string, string> &best_genes, double score, vector<RecoEvent> *events) {
  RecoEvent event(FillRecoEvent(seqs, kset, best_genes, score));
  events->push_back(event);
}

// ----------------------------------------------------------------------------------------
RecoEvent JobHolder::FillRecoEvent(Sequences &seqs, KSet kset, map<string, string> &best_genes, double score) {
  RecoEvent event;
  vector<string> seq_strs(seqs.n_seqs(), "");  // build up these strings summing over each regions
  for(auto & region : gl_.regions_) {
    vector<string> query_strs(GetQueryStrs(seqs, kset, region));
    if(best_genes.find(region) == best_genes.end()) {
      seqs.Print();
    }
    assert(best_genes.find(region) != best_genes.end());
    string gene(best_genes[region]);
    vector<string> path_names = paths_[gene][query_strs]->name_vector();
    if(path_names.size() == 0) {
      if(debug_) cout << "                     " << gene << " has no valid path" << endl;
      event.SetScore(-INFINITY);
      return event;
    }
    assert(path_names.size() > 0);
    assert(path_names.size() == query_strs[0].size());
    event.SetGene(region, gene);

    // set right-hand deletions
    event.SetDeletion(region + "_3p", GetErosionLength("right", path_names, gene));
    // and left-hand deletions
    event.SetDeletion(region + "_5p", GetErosionLength("left", path_names, gene));

    SetInsertions(region, query_strs[0], path_names, &event);  // NOTE this sets the insertion *only* according to the *first* sequence. Which makes sense at the moment, since the RecoEvent class is only designed to represent a single sequence

    for(size_t iseq = 0; iseq < seq_strs.size(); ++iseq)
      seq_strs[iseq] += query_strs[iseq];
  }

  event.SetSeq(seqs[0].name(), seq_strs[0]);
  for(size_t iseq = 1; iseq < seq_strs.size(); ++iseq)
    event.AddAuxiliarySeqs(seqs[iseq].name(), seq_strs[iseq]);
  event.SetScore(score);
  return event;
}

// ----------------------------------------------------------------------------------------
vector<string> JobHolder::GetQueryStrs(Sequences &seqs, KSet kset, string region) {
  Sequences query_seqs(GetSubSeqs(seqs, kset, region));
  vector<string> query_strs;
  for(size_t iseq = 0; iseq < seqs.n_seqs(); ++iseq)
    query_strs.push_back(query_seqs[iseq].undigitized());
  return query_strs;
}

// ----------------------------------------------------------------------------------------
void JobHolder::RunKSet(Sequences &seqs, KSet kset, map<KSet, double> *best_scores, map<KSet, double> *total_scores, map<KSet, map<string, string> > *best_genes) {
  map<string, Sequences> subseqs(GetSubSeqs(seqs, kset));
  (*best_scores)[kset] = -INFINITY;
  (*total_scores)[kset] = -INFINITY;  // total log prob of this kset, i.e. log(P_v * P_d * P_j), where e.g. P_v = \sum_i P(v_i k_v)
  (*best_genes)[kset] = map<string, string>();
  map<string, double> regional_best_scores; // the best score for each region
  map<string, double> regional_total_scores; // the total score for each region, i.e. log P_v
  if(debug_ == 2)
    cout << "            " << kset.v << " " << kset.d << " -------------------" << endl;
  for(auto & region : gl_.regions_) {
    vector<string> query_strs(GetQueryStrs(seqs, kset, region));

    TermColors tc;
    if(debug_ == 2) {
      if(algorithm_ == "viterbi") {
        cout << "              " << region << " query " << query_strs[0] << endl;
        for(size_t is = 1; is < query_strs.size(); ++is)
          cout << "              " << region << " query " << tc.ColorMutants("purple", query_strs[is], query_strs[0]) << endl;  // use the first query_str as reference sequence... could just as well use any other
      } else {
        cout << "              " << region << endl;
      }
    }

    regional_best_scores[region] = -INFINITY;
    regional_total_scores[region] = -INFINITY;
    size_t igene(0), n_short_v(0), n_long_erosions(0);
    for(auto & gene : gl_.names_[region]) {
      if(only_genes_[region].size() > 0 and only_genes_[region].find(gene) == only_genes_[region].end())
        continue;
      igene++;

      if(region == "v" && query_strs[0].size() > gl_.seqs_[gene].size()) { // query sequence too long for this v version to make any sense (ds and js have inserts so this doesn't affect them)
        if(debug_ == 2) cout << "                     " << gene << " too short" << endl;
        n_short_v++;
        continue;
      }
      if(query_strs[0].size() < gl_.seqs_[gene].size() - 10)   // entry into the left side of the v hmm is a little hacky, and is governed by a gaussian with width 5 (hmmwriter::fuzz_around_v_left_edge)
        n_long_erosions++;

      double *gene_score(&all_scores_[gene][query_strs]);  // pointed-to value is already set if we have this trellis cached, otherwise not
      bool already_cached = trellisi_.find(gene) != trellisi_.end() && trellisi_[gene].find(query_strs) != trellisi_[gene].end();
      string origin("ARG");
      if(already_cached) {
        origin = "cached";
      } else {
        FillTrellis(subseqs[region], query_strs, gene, gene_score, origin);  // sets *gene_score to uncorrected score
        double gene_choice_score = log(hmms_.Get(gene, debug_)->overall_prob());
        *gene_score = AddWithMinusInfinities(*gene_score, gene_choice_score);  // then correct it for gene choice probs
      }
      if(debug_ == 2 && algorithm_ == "viterbi")
        PrintPath(query_strs, gene, *gene_score, origin);

      // set regional total scores
      regional_total_scores[region] = AddInLogSpace(*gene_score, regional_total_scores[region]);  // (log a, log b) --> log a+b, i.e. here we are summing probabilities in log space, i.e. a *or* b
      if(debug_ == 2 && algorithm_ == "forward")
        printf("                %9.2f (%.1e)  tot: %7.2f  %s\n", *gene_score, exp(*gene_score), regional_total_scores[region], tc.ColorGene(gene).c_str());

      // set best regional scores
      if(*gene_score > regional_best_scores[region]) {
        regional_best_scores[region] = *gene_score;
        (*best_genes)[kset][region] = gene;
      }

      // set best per-gene scores
      if(best_per_gene_scores_.find(gene) == best_per_gene_scores_.end())
        best_per_gene_scores_[gene] = -INFINITY;
      if(*gene_score > best_per_gene_scores_[gene])
        best_per_gene_scores_[gene] = *gene_score;

    }

    // return if we didn't find a valid path for this region
    if((*best_genes)[kset].find(region) == (*best_genes)[kset].end()) {
      if(debug_ == 2) {
        cout << "                  found no gene for " << region << " so skip"
             << " (" << n_short_v << "/" << igene << " v germlines too short, " << n_long_erosions << "/" << igene << " would require more than 10 erosions)" << endl;
      }
      return;
    }
  }

  (*best_scores)[kset] = AddWithMinusInfinities(regional_best_scores["v"], AddWithMinusInfinities(regional_best_scores["d"], regional_best_scores["j"]));  // i.e. best_prob = v_prob * d_prob * j_prob (v *and* d *and* j)
  (*total_scores)[kset] = AddWithMinusInfinities(regional_total_scores["v"], AddWithMinusInfinities(regional_total_scores["d"], regional_total_scores["j"]));
}

// ----------------------------------------------------------------------------------------
void JobHolder::SetInsertions(string region, string query_str, vector<string> path_names, RecoEvent *event) {
  Insertions ins;
  for(auto & insertion : ins[region]) {  // loop over the boundaries (vd and dj)
    string side(insertion == "jf" ? "right" : "left");
    size_t length(GetInsertLength(side, path_names));
    string inserted_bases = query_str.substr(GetInsertStart(side, path_names.size(), length), length);
    event->SetInsertion(insertion, inserted_bases);
  }
}

// ----------------------------------------------------------------------------------------
size_t JobHolder::GetInsertStart(string side, size_t path_length, size_t insert_length) {
  if(side == "left") {
    return 0;
  } else if(side == "right") {
    return path_length - insert_length;
  } else {
    throw runtime_error("ERROR side must be left or right, not \"" + side + "\"");
  }

}

// ----------------------------------------------------------------------------------------
size_t JobHolder::GetInsertLength(string side, vector<string> names) {
  size_t n_inserts(0);
  if(side == "left") {
    for(auto & name : names) {
      if(name.find("insert") == 0)
        ++n_inserts;
      else
        break;
    }
  } else if(side == "right") {
    for(size_t ip = names.size() - 1; ip >= 0; --ip)
      if(names[ip].find("insert") == 0)
        ++n_inserts;
      else
        break;
  } else {
    throw runtime_error("ERROR side must be left or right, not \"" + side + "\"");
  }

  return n_inserts;
}

// ----------------------------------------------------------------------------------------
size_t JobHolder::GetErosionLength(string side, vector<string> names, string gene_name) {
  string germline(gl_.seqs_[gene_name]);

  // first check if we eroded the entire sequence. If so we can't say how much was left and how much was right, so just (integer) divide by two (arbitrarily giving one side the odd base if necessary)
  bool its_inserts_all_the_way_down(true);
  for(auto & name : names) {
    if(name.find("insert") != 0) {
      assert(name.find("IGH") == 0);  // Trust but verify, my little ducky, trust but verify.
      its_inserts_all_the_way_down = false;
      break;
    }
  }
  if(its_inserts_all_the_way_down) {   // entire sequence is inserts, so there's no way to tell which part is a left erosion and which is a right erosion
    if(side == "left")
      return floor(float(germline.size()) / 2);
    else if(side == "right")
      return ceil(float(germline.size()) / 2);
    else
      throw runtime_error("ERROR bad side: " + side);
  }

  // find the index in <names> up to which we eroded
  size_t istate(0);  // index (in path) of first non-eroded state
  if(side == "left") { // to get left erosion length we look at the first non-insert state in the path
    for(size_t il = 0; il < names.size(); ++il) { // loop over each state from left to right
      if(names[il].find("insert") == 0) {   // skip any insert states on the left
        continue;
      } else {  // found the leftmost non-insert state -- that's the one we want
        istate = il;
        break;
      }
    }
  } else if(side == "right") { // and for the righthand one we need the last non-insert state
    for(size_t il = names.size() - 1; il >= 0; --il) {
      if(names[il].find("insert") == 0) {   // skip any insert states on the left
        continue;
      } else {  // found the leftmost non-insert state -- that's the one we want
        istate = il;
        break;
      }
    }
  } else {
    assert(0);
  }

  // then find the state number (in the hmm's state numbering scheme) of the state found at that index in the viterbi path
  assert(istate >= 0 && istate < names.size());
  if(names[istate].find("IGH") != 0)  // start of state name should be IGH[VDJ]
    throw runtime_error("ERROR state not of the form IGH<gene>_<position>: " + names[istate]);
  assert(names[istate].find("IGH") == 0);  // start of state name should be IGH[VDJ]
  string state_index_str = names[istate].substr(names[istate].find_last_of("_") + 1);
  size_t state_index = atoi(state_index_str.c_str());

  size_t length(0);
  if(side == "left") {
    length = state_index;
  } else if(side == "right") {
    size_t germline_length = gl_.seqs_[gene_name].size();
    length = germline_length - state_index - 1;
  } else {
    assert(0);
  }

  return length;
}

// ----------------------------------------------------------------------------------------
void JobHolder::WriteBestGeneProbs(ofstream &ofs, string query_name) {
  ofs << query_name << ",";
  stringstream ss;
  for(auto & gene_map : best_per_gene_scores_)
    ss << gene_map.first << ":" << gene_map.second << ";";
  ofs << ss.str().substr(0, ss.str().size() - 1) << endl; // remove the last semicolon
}
