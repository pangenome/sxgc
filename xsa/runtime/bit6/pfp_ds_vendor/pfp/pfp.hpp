/* pfp - prefix free parsing
    Copyright (C) 2020 Massimiliano Rossi

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see http://www.gnu.org/licenses/ .
*/
/*!
   \file pfp.hpp
   \brief pfp.hpp define and build the prefix-free parsing data structures.
   \author Massimiliano Rossi
   \date 03/04/2020
*/


#ifndef _PFP_HH
#define _PFP_HH

#include <sdsl/rmq_support.hpp>
#include <sdsl/int_vector.hpp>
#include <sdsl/wavelet_trees.hpp>

#include "utils.hpp"
#include "dictionary.hpp"
#include "parse.hpp"
#include "wt.hpp"

namespace pfpds
{

template <typename dict_data_type, typename colex_comparator_type = std::less<dict_data_type>, class wt_t = pfp_wt_custom>
class pf_parsing
{
public:
    struct M_entry_t
    {
        long_type len;
        long_type left; // left and right are the extremes of the range
        long_type right;
    };
    
    // references to dictionary and parse
    const dictionary<dict_data_type, colex_comparator_type>& dict;
    
    const parse& pars;
    
    // data structures stored in the pfp class
    long_type n; // Size of the text
    long_type w; // Size of the window
    
    std::vector<long_type> freq;
    
    sdsl::bit_vector b_bwt;
    
    sdsl::bit_vector::rank_1_type b_bwt_rank_1;
    
    sdsl::bit_vector::select_1_type b_bwt_select_1;
    
    std::vector<M_entry_t> M;
    
    wt_t w_wt;
    
    std::vector<sdsl::int_vector<0>> bwt_p_ilist;
    
    sdsl::bit_vector b_p;
    
    sdsl::bit_vector::rank_1_type rank_b_p;
    
    sdsl::bit_vector::select_1_type select_b_p;
    
    bool W_flag = false;
    bool bwt_P_ilist_flag = false;
    
    pf_parsing(
    const dictionary<dict_data_type, colex_comparator_type>& d,
    const parse& p,
    bool W_flag_ = false,
    bool bwt_P_ilist_flag_ = false)
    :dict(d), pars(p), w(dict.w), freq(d.n_phrases() + 1, 0)
    {
        // compute frequencies
        for (long_type i = 0; i < pars.p.size() - 1; i++) { freq[pars.p[i]] += 1; } // p ends with 0
        assert(freq[0] == 0);
        
        // Compute the length of the string;
        compute_n();
        
        spdlog::info("Computing b_p");
        compute_b_p();
        
        spdlog::info("Computing b_bwt and M of the parsing");
        build_b_bwt_and_M();
        
        if (W_flag_)
        {
            spdlog::info("Computing W of BWT(P)");
            build_W();
            W_flag = true;
        }
        
        if (bwt_P_ilist_flag_)
        {
            spdlog::info("Computing inverted list of BWT(P)");
            build_bwt_P_ilist();
            bwt_P_ilist_flag = true;
        }
    }
    
    // ---- load support (bit6/pfp_ds_vendor; one of exactly TWO
    // divergences from upstream r-pfbwt pfp_ds — the other is
    // dictionary.hpp's defer_build_t constructor) ----
    // Tag for constructing a pf_parsing WITHOUT running any build: the
    // caller (bit6/chi_rspace_dump.cpp --pfp-index) is responsible for
    // restoring every member the queries touch: n, w, W_flag, b_bwt
    // (+rank/select), b_p (+rank/select), M, w_wt.  The standard
    // constructor cannot be told to skip build_b_bwt_and_M(), which
    // allocates an O(n)-bit b_bwt and an |M| ~ 0.1n vector — that is the
    // cost this constructor exists to avoid.
    struct defer_build_t {};

    pf_parsing(
    const dictionary<dict_data_type, colex_comparator_type>& d,
    const parse& p,
    defer_build_t)
    :dict(d), pars(p), w(dict.w)
    { }

    void
    compute_b_p()
    {
        // Build the bitvector storing the position of the beginning of each phrase.
        b_p.resize(this->n); // all should be initialized at false by sdsl
        for (long_type i = 0; i < b_p.size(); ++i)
            b_p[i] = false; // bug in resize
        b_p[0] = true; // phrase_0 becomes phrase 1
        
        long_type i = 0;
        
        for (long_type j = 0; j < pars.p.size() - 2; ++j)
        { // -2 because the beginning of the last phrase is in position 0
            // p[i]: phrase_id
            assert(pars.p[j] != 0);
            // phrase_length: select_b_d(p[i]+1)-select_b_d(p[i]);
            i += dict.length_of_phrase(pars.p[j]) - w;
            b_p[i] = true;
        }
        
        // Build rank and select on Sp
        rank_b_p = sdsl::bit_vector::rank_1_type(&b_p);
        select_b_p = sdsl::bit_vector::select_1_type(&b_p);
    }
    
    void
    compute_n()
    {
        // Compute the length of the string;
        n = 0;
        for (long_type j = 0; j < pars.p.size() - 1; ++j)
        {
            // parse.p[j]: phrase_id
            assert(pars.p[j] != 0);
            n += dict.length_of_phrase(pars.p[j]) - w;
        }
        //n += w; // + w because n is the length including the last w markers
        //n += w - 1; // Changed after changind b_d in dict // -1 is for the first dollar + w because n is the length including the last w markers
    }
    
    void
    build_b_bwt_and_M()
    {
        assert(dict.saD_flag);
        assert(dict.lcpD_flag);
        assert(dict.daD_flag);
        assert(dict.colex_daD_flag);
        
        b_bwt.resize(n);
        for (long_type i = 0; i < b_bwt.size(); ++i) { b_bwt[i] = false; } // bug in resize
        
        assert(dict.d[dict.saD[0]] == EndOfDict);
        long_type i = 1; // This should be safe since the first entry of sa is always the dollarsign used to compute the sa
        long_type j = 0;
        
        while (i < dict.saD.size())
        {
            long_type left = i;
            
            auto sn = dict.saD[i];
            // Check if the suffix has length at least w and is not the complete phrase.
            auto phrase = dict.daD[i] + 1;
            assert(phrase > 0 && phrase < freq.size()); // + 1 because daD is 0-based
            long_type suffix_length = dict.select_b_d(dict.rank_b_d(sn + 1) + 1) - sn - 1;
            if (dict.b_d[sn] || suffix_length < dict.w)
            {
                ++i; // Skip
            }
            else
            {
                // use the RMQ data structure to find how many of the following suffixes are the same except for the terminator (so they're the same suffix but in different phrases)
                // use the document array and the table of phrase frequencies to find the phrases frequencies and sum them up
                b_bwt[j++] = true;
                j += freq[phrase] - 1; // the next bits are 0s
                i++;
                
                if (i < dict.saD.size())
                {
                    auto new_sn = dict.saD[i];
                    auto new_phrase = dict.daD[i] + 1;
                    assert(new_phrase > 0 && new_phrase < freq.size()); // + 1 because daD is 0-based
                    long_type new_suffix_length = dict.select_b_d(dict.rank_b_d(new_sn + 1) + 1) - new_sn - 1;
                    
                    while (i < dict.saD.size() && (dict.lcpD[i] >= suffix_length)
                    && (suffix_length == new_suffix_length))
                    {
                        j += freq[new_phrase];
                        ++i;
                        
                        if (i < dict.saD.size())
                        {
                            new_sn = dict.saD[i];
                            new_phrase = dict.daD[i] + 1;
                            assert(new_phrase > 0 && new_phrase < freq.size()); // + 1 because daD is 0-based
                            new_suffix_length = dict.select_b_d(dict.rank_b_d(new_sn + 1) + 1) - new_sn - 1;
                        }
                    }
                }
                
                // Computing M
                long_type right = i - 1;
                M_entry_t m;
                m.len = suffix_length;
                m.left = dict.colex_daD[dict.rmq_colex_daD(left, right)];
                m.right = dict.colex_daD[dict.rMq_colex_daD(left, right)];
                
                M.emplace_back(m);
            }
        }
        
        // rank & select support for b_bwt
        b_bwt_rank_1 = sdsl::bit_vector::rank_1_type(&b_bwt);
        b_bwt_select_1 = sdsl::bit_vector::select_1_type(&b_bwt);
    }
    
    void
    build_W()
    {
        assert(dict.colex_id_flag);
        assert(pars.saP_flag);
        
        // create alphabet (phrases)
        std::vector<uint32_t> alphabet(dict.n_phrases());
        for (long_type i = 0; i < dict.n_phrases(); ++i)
        {
            alphabet[i] = dict.colex_id[i] + 1;
        }
        
        // create BWT(P)
        std::vector<uint32_t> bwt_p(pars.p.size() - 1, 0);
        for (long_type i = 1; i < pars.saP.size(); ++i) // TODO: shoud we count end symbol in this?
        {
            if (pars.saP[i] > 0) { bwt_p[i - 1] = pars.p[pars.saP[i] - 1]; }
            else { bwt_p[i - 1] = pars.p[pars.p.size() - 2]; } // TODO: this should be -1 only if 0 stay in pars
        }
        
        w_wt.construct(alphabet, bwt_p);
    }
    
    void
    build_bwt_P_ilist()
    {
        assert(pars.saP_flag);
        
        // allocate lists
        bwt_p_ilist.resize(dict.n_phrases() + 1);
        
        long_type bytes_ilist = 0;
        long_type bwt_length = pars.p.size();
        while (bwt_length != 0) { bwt_length >>= 8; bytes_ilist++; }
        spdlog::info("Using {} bytes for storing the inverted list of the BWT of the parse", bytes_ilist);
        
        for (long_type i = 1; i < dict.n_phrases() + 1; i++)
        {
            bwt_p_ilist[i] = sdsl::int_vector<>(freq[i], 0ULL, bytes_ilist * 8);
        }
        
        // create BWT(P)
        std::vector<long_type> ilists_iterators(dict.n_phrases() + 1, 0);
        for (long_type i = 1; i < pars.saP.size(); ++i) // TODO: shoud we count end symbol in this?
        {
            if (pars.saP[i] > 0)
            {
                auto& il_it = ilists_iterators[pars.p[pars.saP[i] - 1]];
                bwt_p_ilist[pars.p[pars.saP[i] - 1]][il_it] = i - 1;
                il_it++;
            }
            else
            {
                auto& il_it = ilists_iterators[pars.p[pars.p.size() - 2]];
                bwt_p_ilist[pars.p[pars.p.size() - 2]][ilists_iterators[il_it]] = i - 1;
                il_it++;
            }
        }
    }
};

}


#endif /* end of include guard: _PFP_HH */
