#include "html/tokenizer.h"
#include <cctype>
#include <set>

static std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

char HtmlTokenizer::peek(int off) const {
    size_t i = m_pos + off;
    return i < m_src->size() ? (*m_src)[i] : '\0';
}

char HtmlTokenizer::consume() {
    return m_pos < m_src->size() ? (*m_src)[m_pos++] : '\0';
}

bool HtmlTokenizer::startsWith(const std::string& s) const {
    return m_src->compare(m_pos, s.size(), s) == 0;
}

void HtmlTokenizer::skipWS() {
    while (m_pos < m_src->size() && std::isspace((unsigned char)(*m_src)[m_pos]))
        m_pos++;
}

std::string HtmlTokenizer::consumeName() {
    std::string name;
    while (m_pos < m_src->size()) {
        char c = (*m_src)[m_pos];
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':')
            name += consume();
        else break;
    }
    return toLower(name);
}

std::string HtmlTokenizer::decodeEntities(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ) {
        if (raw[i] != '&') { out += raw[i++]; continue; }
        size_t semi = raw.find(';', i + 1);
        if (semi == std::string::npos) { out += raw[i++]; continue; }
        std::string ent = raw.substr(i + 1, semi - i - 1);
        if      (ent == "amp")  { out += '&';  i = semi + 1; }
        else if (ent == "lt")   { out += '<';  i = semi + 1; }
        else if (ent == "gt")   { out += '>';  i = semi + 1; }
        else if (ent == "quot") { out += '"';  i = semi + 1; }
        else if (ent == "apos") { out += '\''; i = semi + 1; }
        else if (ent == "nbsp") { out += ' ';  i = semi + 1; }
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = 0;
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                cp = std::stoul(ent.substr(2), nullptr, 16);
            else
                cp = std::stoul(ent.substr(1));
            // Encode as UTF-8
            if (cp < 0x80)       out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0|(cp>>6)); out += (char)(0x80|(cp&0x3F)); }
            else { out += (char)(0xE0|(cp>>12)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
            i = semi + 1;
        } else { out += raw[i++]; }
    }
    return out;
}

std::map<std::string, std::string> HtmlTokenizer::consumeAttrs() {
    std::map<std::string, std::string> attrs;
    while (m_pos < m_src->size() &&
           (*m_src)[m_pos] != '>' && (*m_src)[m_pos] != '/') {
        skipWS();
        if (m_pos >= m_src->size() || (*m_src)[m_pos] == '>' || (*m_src)[m_pos] == '/') break;
        std::string name = consumeName();
        if (name.empty()) { consume(); continue; }
        std::string value;
        skipWS();
        if (m_pos < m_src->size() && (*m_src)[m_pos] == '=') {
            consume();
            skipWS();
            if (m_pos < m_src->size()) {
                char q = (*m_src)[m_pos];
                if (q == '"' || q == '\'') {
                    consume();
                    while (m_pos < m_src->size() && (*m_src)[m_pos] != q)
                        value += consume();
                    if (m_pos < m_src->size()) consume();
                } else {
                    while (m_pos < m_src->size()
                        && !std::isspace((unsigned char)(*m_src)[m_pos])
                        && (*m_src)[m_pos] != '>')
                        value += consume();
                }
            }
        }
        attrs[name] = value;
    }
    return attrs;
}

void HtmlTokenizer::skipUntil(const std::string& stop) {
    while (m_pos < m_src->size() && !startsWith(stop))
        m_pos++;
}

void HtmlTokenizer::tokenize(const std::string& html, Callback cb) {
    m_src = &html;
    m_pos = 0;

    // Tags whose content we skip entirely
    static const std::set<std::string> rawTags = { "script", "style", "noscript" };

    while (m_pos < m_src->size()) {
        if ((*m_src)[m_pos] != '<') {
            // Collect raw text up to next '<'
            std::string raw;
            while (m_pos < m_src->size() && (*m_src)[m_pos] != '<')
                raw += consume();
            HtmlToken t;
            t.type = TokenType::Text;
            t.data = decodeEntities(raw);
            cb(t);
            continue;
        }

        consume(); // '<'
        if (m_pos >= m_src->size()) break;

        // Comments
        if (startsWith("!--")) {
            m_pos += 3;
            skipUntil("-->");
            m_pos += 3;
            continue;
        }
        // DOCTYPE
        if (toLower(m_src->substr(m_pos, 7)) == "doctype") {
            skipUntil(">");
            if (m_pos < m_src->size()) consume();
            HtmlToken t; t.type = TokenType::Doctype; cb(t);
            continue;
        }
        // CDATA
        if (startsWith("![CDATA[")) {
            m_pos += 8;
            skipUntil("]]>");
            m_pos += 3;
            continue;
        }

        bool isEnd = false;
        if ((*m_src)[m_pos] == '/') { isEnd = true; consume(); }

        skipWS();
        std::string tagName = consumeName();
        if (tagName.empty()) {
            // Malformed tag — skip to >
            skipUntil(">");
            if (m_pos < m_src->size()) consume();
            continue;
        }

        auto attrs = isEnd ? std::map<std::string,std::string>{}
                           : consumeAttrs();
        skipWS();
        bool selfClose = false;
        if (m_pos < m_src->size() && (*m_src)[m_pos] == '/') { selfClose = true; consume(); }
        if (m_pos < m_src->size() && (*m_src)[m_pos] == '>') consume();

        HtmlToken t;
        t.type        = isEnd ? TokenType::EndTag : TokenType::StartTag;
        t.name        = tagName;
        t.attrs       = std::move(attrs);
        t.selfClosing = selfClose;
        cb(t);

        // Skip raw-content tags
        if (!isEnd && rawTags.count(tagName)) {
            skipUntil("</" + tagName);
        }
    }

    HtmlToken eof; eof.type = TokenType::EOF_; cb(eof);
}
