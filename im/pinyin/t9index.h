/*
 * SPDX-FileCopyrightText: 2026 Fcitx5 for Android Contributors
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef _PINYIN_T9INDEX_H_
#define _PINYIN_T9INDEX_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <libime/pinyin/pinyindictionary.h>

namespace fcitx {

/**
 * T9 (nine key) support for full pinyin.
 *
 * The digit sequence is resolved against the static pinyin syllable table, not
 * against the dictionary: every digit span that spells a legal syllable is an
 * edge, and every path through that graph is handed to the ordinary
 * PinyinDictionary lookup. The number of candidates therefore grows with the
 * number of syllables in the input rather than with the number of letter
 * combinations, and the existing dictionary costs and language model keep
 * doing the ranking.
 */
class T9Index {
public:
    /// A pinyin entry matched by the current digit sequence.
    struct Match {
        /// Encoded pinyin of the matched entry, same encoding as the
        /// dictionary key (two bytes per syllable plus a separator).
        std::string encodedPinyin;
        /// The word itself.
        std::string word;
        /// Dictionary cost, lower is more frequent.
        float cost = 0.0F;
    };

    /**
     * Map a single latin letter to its T9 digit.
     *
     * @return the digit in '2'..'9', or 0 when the letter has no mapping.
     */
    static char letterToDigit(char letter);

    /**
     * Map a lower case full pinyin string to its T9 digit signature.
     *
     * Characters other than a-z (for example the apostrophe used as a syllable
     * separator) are ignored.
     */
    static std::string digitSignature(std::string_view pinyin);

    /**
     * Query the dictionary for entries matching a digit sequence.
     *
     * @param dictionary the dictionary to search.
     * @param digits the digit sequence, may be empty.
     * @param prefix also report entries whose pinyin starts with a complete
     *               match of the digit sequence, used while the user is still
     *               typing.
     * @param maxResult upper bound on the number of returned entries.
     */
    std::vector<Match> query(const libime::PinyinDictionary &dictionary,
                             std::string_view digits, bool prefix,
                             size_t maxResult) const;

    /**
     * Longest digit sequence accepted by a single query.
     *
     * Bounds the syllable graph so that a long keypad mash cannot make the
     * query explode.
     */
    static size_t maxQueryLength();

    /**
     * Longest number of digits a single pinyin syllable can occupy, derived
     * from the syllable table. This is what keeps the syllable graph narrow.
     */
    static size_t maxSyllableDigits();
};

} // namespace fcitx

#endif // _PINYIN_T9INDEX_H_
