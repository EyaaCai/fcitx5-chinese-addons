/*
 * SPDX-FileCopyrightText: 2026 Fcitx5 for Android Contributors
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "t9index.h"
#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <libime/pinyin/pinyindata.h>
#include <libime/pinyin/pinyinencoder.h>

namespace fcitx {

namespace {

/**
 * Separator used between syllables in the encoded dictionary key.
 *
 * Mirrors libime's file local pinyinHanziSep (pinyindictionary.cpp), which is
 * not exported, so the value has to be repeated here. Must stay in sync when
 * the libime submodule is updated.
 */
constexpr char encodedSyllableSep = '!';

/// Upper bound on the digit sequence accepted by one query.
constexpr size_t maxDigits = 32;

/// One digit span that spells at least one legal syllable.
struct SyllableEdge {
    size_t begin = 0;
    size_t end = 0;
    /// Encoded forms of every syllable spelling this span.
    std::vector<std::string> encodedSyllables;
};

struct LookupTable {
    /// digit substring -> encoded forms of every syllable spelling it.
    std::unordered_map<std::string, std::vector<std::string>> syllables;
    size_t maxSyllableDigits = 0;
};

/// Encode one syllable the same way the dictionary key does.
std::string encodeSyllable(const libime::PinyinEntry &entry) {
    std::string encoded;
    encoded.push_back(static_cast<char>(entry.initial()));
    encoded.push_back(static_cast<char>(entry.final()));
    encoded.push_back(encodedSyllableSep);
    return encoded;
}

/**
 * Build the digit -> syllable table from the static pinyin syllable table.
 *
 * The syllable table is compiled into libime and never changes at runtime, so
 * this is built once and shared by every query.
 */
const LookupTable &lookupTable() {
    static const LookupTable table = [] {
        LookupTable result;
        for (const auto &entry : libime::getPinyinMapV2()) {
            const auto signature = T9Index::digitSignature(entry.pinyinView());
            if (signature.empty()) {
                continue;
            }
            result.maxSyllableDigits =
                std::max(result.maxSyllableDigits, signature.size());
            auto &encoded = result.syllables[signature];
            auto form = encodeSyllable(entry);
            // Several spellings can share one signature, and the same spelling
            // may appear under different fuzzy flags, so deduplicate.
            if (std::find(encoded.begin(), encoded.end(), form) ==
                encoded.end()) {
                encoded.push_back(std::move(form));
            }
        }
        return result;
    }();
    return table;
}

/**
 * All digit spans of @p digits that spell a legal syllable, ordered by start
 * position so that a depth first walk yields candidates in input order.
 */
std::vector<SyllableEdge> edgesFor(std::string_view digits) {
    const auto &table = lookupTable();
    std::vector<SyllableEdge> edges;
    // Every syllable occupies at least one digit, so a syllable starting at
    // `begin` spans at most maxSyllableDigits digits.
    for (size_t begin = 0; begin < digits.size(); begin++) {
        const auto maxEnd =
            std::min(digits.size(), begin + table.maxSyllableDigits);
        for (size_t end = begin + 1; end <= maxEnd; end++) {
            auto iter = table.syllables.find(
                std::string(digits.substr(begin, end - begin)));
            if (iter == table.syllables.end()) {
                continue;
            }
            SyllableEdge edge;
            edge.begin = begin;
            edge.end = end;
            edge.encodedSyllables = iter->second;
            edges.push_back(std::move(edge));
        }
    }
    std::sort(edges.begin(), edges.end(),
              [](const SyllableEdge &lhs, const SyllableEdge &rhs) {
                  return std::tie(lhs.begin, lhs.end) <
                         std::tie(rhs.begin, rhs.end);
              });
    return edges;
}

using Match = T9Index::Match;

/// Key used to drop entries reached through more than one syllable split.
std::string dedupKey(const std::string &encodedPinyin,
                     const std::string &word) {
    std::string key = encodedPinyin;
    key.push_back('\0');
    key.append(word);
    return key;
}

/**
 * Walk the syllable graph depth first and collect dictionary entries.
 *
 * @param encoded encoded pinyin of the syllables chosen so far, with the
 *                trailing separator for each, i.e. a valid dictionary prefix.
 */
void collect(const libime::PinyinDictionary &dictionary, std::string_view digits,
             const std::vector<SyllableEdge> &edges, size_t position,
             std::string &encoded, bool prefix, size_t maxResult,
             std::unordered_set<std::string> &seen,
             std::vector<Match> &result) {
    if (result.size() >= maxResult) {
        return;
    }
    auto append = [&](std::string_view encodedPinyin, std::string_view word,
                      float cost) {
        if (result.size() >= maxResult) {
            return false;
        }
        // A prefix match must end exactly on a syllable boundary, otherwise
        // 64 would also "match" 644 (ni -> ni'...).
        if (encodedPinyin.size() < encoded.size()) {
            return true;
        }
        if (!prefix &&
            (encodedPinyin.size() != encoded.size() ||
             encodedPinyin != encoded)) {
            return true;
        }
        if (prefix && encodedPinyin.size() > encoded.size() &&
            encodedPinyin[encoded.size()] != encodedSyllableSep) {
            return true;
        }
        auto key = dedupKey(std::string(encodedPinyin), std::string(word));
        if (!seen.insert(std::move(key)).second) {
            return true;
        }
        Match match;
        match.encodedPinyin = encodedPinyin;
        match.word = word;
        match.cost = cost;
        result.push_back(std::move(match));
        return true;
    };

    if (position == digits.size()) {
        // Complete digit sequence: entries equal to it, plus, when prefix
        // matching is on, longer entries continuing on a syllable boundary.
        dictionary.matchWords(encoded.data(), encoded.size(), append);
        if (prefix) {
            dictionary.matchWordsPrefix(encoded.data(), encoded.size(), append);
        }
        return;
    }

    for (const auto &edge : edges) {
        if (edge.begin != position || result.size() >= maxResult) {
            continue;
        }
        for (const auto &syllable : edge.encodedSyllables) {
            if (result.size() >= maxResult) {
                break;
            }
            encoded.append(syllable);
            collect(dictionary, digits, edges, edge.end, encoded, prefix,
                    maxResult, seen, result);
            encoded.resize(encoded.size() - syllable.size());
        }
    }
}

} // namespace

char T9Index::letterToDigit(char letter) {
    // Standard ITU E.161 keypad layout. The rest of the pinyin engine already
    // works on lower case input, so upper case is not handled here.
    switch (letter) {
    case 'a':
    case 'b':
    case 'c':
        return '2';
    case 'd':
    case 'e':
    case 'f':
        return '3';
    case 'g':
    case 'h':
    case 'i':
        return '4';
    case 'j':
    case 'k':
    case 'l':
        return '5';
    case 'm':
    case 'n':
    case 'o':
        return '6';
    case 'p':
    case 'q':
    case 'r':
    case 's':
        return '7';
    case 't':
    case 'u':
    case 'v':
        return '8';
    case 'w':
    case 'x':
    case 'y':
    case 'z':
        return '9';
    default:
        return 0;
    }
}

std::string T9Index::digitSignature(std::string_view pinyin) {
    std::string signature;
    signature.reserve(pinyin.size());
    for (auto letter : pinyin) {
        if (auto digit = letterToDigit(letter)) {
            signature.push_back(digit);
        }
    }
    return signature;
}

size_t T9Index::maxSyllableDigits() {
    return lookupTable().maxSyllableDigits;
}

size_t T9Index::maxQueryLength() { return maxDigits; }

std::vector<T9Index::Match>
T9Index::query(const libime::PinyinDictionary &dictionary,
               std::string_view digits, bool prefix, size_t maxResult) const {
    std::vector<Match> result;
    if (digits.empty() || maxResult == 0 || digits.size() > maxDigits) {
        return result;
    }
    // Reject anything that is not a keypad digit, so that a malformed sequence
    // cannot silently match a shortened signature.
    if (std::any_of(digits.begin(), digits.end(),
                    [](char c) { return c < '2' || c > '9'; })) {
        return result;
    }
    const auto edges = edgesFor(digits);
    if (edges.empty()) {
        return result;
    }
    std::string encoded;
    encoded.reserve(digits.size() * 3);
    std::unordered_set<std::string> seen;
    collect(dictionary, digits, edges, 0, encoded, prefix, maxResult, seen,
            result);
    return result;
}

} // namespace fcitx
