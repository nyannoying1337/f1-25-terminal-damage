// The byte signatures the addon finds its hook sites with (mod/native/src: the umbrella, probe/*.inc and the modules
// *.hpp, read as text): every string literal made only of hex digits, '?' and spaces is a signature and must be
// "48 8B ?? 05 ..." - tokens of two hex digits or "??", one space between them, at most one trailing space (the
// adjacent pieces of a split literal join into one) - with at least 8 bytes that are not wildcards, and no signature
// may appear twice. A typo (one digit, a missing or doubled space, a single '?') would otherwise show up only in the
// game, as a hook that is not installed. Run from the repository root (the test loop does).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "test_util.hpp"

namespace {
struct Literal {
    std::string file;
    int line;
    std::string text;
};

bool is_word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// The string literals of one source text. Comments and character literals are skipped, the literals that follow each
// other with only blanks and comments between them join into one (as the compiler does). Raw literals are read too.
std::vector<Literal> string_literals(const std::string &src, const std::string &file)
{
    std::vector<Literal> out;
    int line = 1;
    bool adjacent = false;  // nothing but blanks and comments since the last literal ended
    size_t i = 0;
    while (i < src.size()) {
        char c = src[i];
        if (c == '\n') {
            ++line;
            ++i;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            ++i;
        } else if (src.compare(i, 2, "//") == 0) {
            while (i < src.size() && src[i] != '\n')
                ++i;
        } else if (src.compare(i, 2, "/*") == 0) {
            size_t end = src.find("*/", i + 2);
            end = end == std::string::npos ? src.size() : end + 2;
            line += int(std::count(src.begin() + long(i), src.begin() + long(end), '\n'));
            i = end;
        } else if (c == '\'' && (i == 0 || !is_word(src[i - 1]))) {  // a character literal (not a digit separator)
            for (++i; i < src.size() && src[i] != '\'' && src[i] != '\n';)
                i += src[i] == '\\' ? 2 : 1;
            ++i;
            adjacent = false;
        } else if (c == '"') {
            int first_line = line;
            std::string text;
            bool raw = i > 0 && src[i - 1] == 'R' && (i == 1 || !is_word(src[i - 2]) || std::string("Lu8U").find(src[i - 2]) != std::string::npos);
            if (raw) {
                size_t open = src.find('(', i);
                assert(open != std::string::npos);
                std::string close = ")" + src.substr(i + 1, open - i - 1) + "\"";
                size_t end = src.find(close, open);
                assert(end != std::string::npos);
                text = src.substr(open + 1, end - open - 1);
                line += int(std::count(text.begin(), text.end(), '\n'));
                i = end + close.size();
            } else {
                for (++i; i < src.size() && src[i] != '"' && src[i] != '\n'; ++i) {
                    if (src[i] == '\\' && i + 1 < src.size()) {
                        text += src[i++];  // an escape stays as written: a signature has none
                    }
                    text += src[i];
                }
                assert(i < src.size() && src[i] == '"');  // a literal ends on its line
                ++i;
            }
            if (adjacent && !out.empty())
                out.back().text += text;
            else
                out.push_back({file, first_line, text});
            adjacent = true;
        } else {
            adjacent = false;
            ++i;
        }
    }
    return out;
}

// Only hex digits, '?' and spaces, at least 8 characters: a signature or a typo in one.
bool signature_like(const std::string &s)
{
    return s.size() >= 8 && std::all_of(s.begin(), s.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) || c == '?' || c == ' ';
    });
}

// "" when the signature is well formed, else what is wrong with it.
std::string syntax_problem(const std::string &s)
{
    std::string body = s;
    if (!body.empty() && body.back() == ' ')
        body.pop_back();  // one trailing space is how a split literal joins
    int bytes = 0;
    for (size_t at = 0;;) {
        size_t end = body.find(' ', at);
        std::string token = body.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (token.empty())
            return "a space too many (exactly one between tokens, at most one at the end, none at the start)";
        if (token.size() == 2 && std::isxdigit(static_cast<unsigned char>(token[0])) &&
            std::isxdigit(static_cast<unsigned char>(token[1])))
            ++bytes;
        else if (token != "??")
            return "bad token '" + token + "' (a byte is two hex digits, a wildcard is ??, one space between them)";
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    if (bytes < 8)
        return "only " + std::to_string(bytes) + " bytes that are not wildcards (at least 8, or it is not unique)";
    return "";
}

std::string normalized(const std::string &s)
{
    std::string r = s;
    if (!r.empty() && r.back() == ' ')
        r.pop_back();
    for (char &c : r)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

// Every problem in the signatures of the given sources; count gets the number of signatures found.
std::vector<std::string> check(const std::vector<std::pair<std::string, std::string>> &files, int &count)
{
    std::vector<std::string> problems;
    std::map<std::string, std::string> seen;  // normalized signature -> where
    count = 0;
    for (const auto &[name, text] : files)
        for (const Literal &lit : string_literals(text, name)) {
            if (!signature_like(lit.text))
                continue;
            ++count;
            std::string where = lit.file + ":" + std::to_string(lit.line);
            if (std::string why = syntax_problem(lit.text); !why.empty())
                problems.push_back(where + ": " + why + ": \"" + lit.text.substr(0, 60) + "\"");
            auto [it, fresh] = seen.emplace(normalized(lit.text), where);
            if (!fresh)
                problems.push_back(where + ": the same signature as " + it->second);
        }
    return problems;
}

std::string slurp(const std::filesystem::path &p)
{
    std::ifstream f(p, std::ios::binary);
    assert(f && "run from the repository root");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

int main()
{
    // ---- the reader and the checker themselves: they must find what is there and reject each kind of typo ----
    {
        std::string src = "const char *a = \"48 8B \"  // the first half\n"
                          "    \"?? 05\" /* \"12 34\" */ \"\\x41\";\n"
                          "const char *b = \"E9 ?? ?? ?? ??\", *c = \"90\" ;\n"
                          "char q = '\"'; const char *d = R\"(8B 05 ?? ?? ?? 90)\";\n"
                          "// \"CC CC CC CC CC CC CC CC\"\n";
        auto lits = string_literals(src, "x");
        assert(lits.size() == 4);
        assert(lits[0].text == "48 8B ?? 05\\x41" && lits[0].line == 1);  // joined over a comment, the comment skipped
        assert(lits[1].text == "E9 ?? ?? ?? ??" && lits[1].line == 3);    // a comma ends the joining
        assert(lits[2].text == "90");
        assert(lits[3].text == "8B 05 ?? ?? ?? 90" && lits[3].line == 4);  // the '"' character literal did not open one
        const std::string good = "48 8B 05 ?? ?? ?? ?? 48 89 5C 24 08 ";  // 8 bytes, one trailing space
        assert(syntax_problem(good).empty());
        assert(syntax_problem(good.substr(0, good.size() - 1)).empty());
        assert(syntax_problem("48 8b 05 ?? ?? ?? ?? 48 89 5c 24 08").empty());  // either case of hex digits
        assert(!syntax_problem("48 8 05 ?? ?? ?? ?? 48 89 5C 24 08").empty());    // one digit
        assert(!syntax_problem("48 8B0 5 ?? ?? ?? ?? 48 89 5C 24 08").empty());   // three digits
        assert(!syntax_problem("488B 05 ?? ?? ?? ?? 48 89 5C 24 08").empty());    // a missing space
        assert(!syntax_problem("48  8B 05 ?? ?? ?? ?? 48 89 5C 24 08").empty());  // two spaces
        assert(!syntax_problem(" 48 8B 05 ?? ?? ?? ?? 48 89 5C 24 08").empty());  // a leading space
        assert(!syntax_problem(good + " ").empty());                              // two trailing spaces
        assert(!syntax_problem("48 8B 05 ? ?? ?? ?? 48 89 5C 24 08").empty());    // a single ?
        assert(!syntax_problem("48 8B 05 ??? ?? ?? 48 89 5C 24 08").empty());     // three ?
        assert(!syntax_problem("48 8B 05 ?? ?? ?? ?? 48 89 5C 24").empty());      // 7 bytes: too short to be unique
        assert(signature_like(good) && !signature_like("48 8B") && !signature_like("lng_ft_detached_wheel"));
        int count = 0;
        assert(check({{"a", "x = \"" + good + "\"; y = \"" + good + "\";"}}, count).size() == 1 && count == 2);  // twice
        assert(check({{"a", "x = \"" + good + "\"; y = \"48 8 05 ?? ?? ?? ?? 48 89 5C 24 08\";"}}, count).size() == 1);
    }

    // ---- the real sources ----
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back("mod/native/src/terminal_damage_probe.cpp", slurp("mod/native/src/terminal_damage_probe.cpp"));
    std::vector<std::filesystem::path> more;
    for (const auto &e : std::filesystem::directory_iterator("mod/native/src/probe"))
        if (e.path().extension() == ".inc")
            more.push_back(e.path());
    for (const auto &e : std::filesystem::directory_iterator("mod/native/src"))
        if (e.path().extension() == ".hpp")
            more.push_back(e.path());
    std::sort(more.begin(), more.end());
    for (const auto &p : more)
        files.emplace_back(p.generic_string(), slurp(p));
    assert(files.size() > 40);

    int count = 0;
    std::vector<std::string> problems = check(files, count);
    for (const std::string &p : problems)
        printf("%s\n", p.c_str());
    printf("%d signatures in %zu files, %zu problems\n", count, files.size(), problems.size());
    assert(problems.empty());
    // The reader is not lost in the sources: these are the signatures there are (a few more may come, not fewer).
    assert(count >= 40);

    // ---- the same sources with one typo each: the check finds it ----
    for (auto &[name, text] : files) {
        size_t at = text.find("\"48 8B ");
        if (at == std::string::npos)
            continue;
        std::string saved = text;
        text.erase(at + 5, 1);  // "48 8B " -> "48 B "
        int typo_count = 0;
        assert(!check(files, typo_count).empty());
        text = saved;
        break;
    }
    puts("PASS: every byte signature is well formed and unique");
}
