// cfront: c_token.cpp —— 关键字表与 token 名称表
//
// 设计要点：关键字表是唯一真相来源。新增一个 C 关键字只需：
//   1) 在 c_token.hpp 的 CTokKind 里加一项
//   2) 在下面的 kKeywords 里加一行
//   3) 在 c_tok_name 的 switch 里加一项（漏了会编译失败，因为开了 -Wswitch）
#include "cfront/c_token.hpp"

#include <unordered_map>

namespace sinter::cfront {
    namespace {
        // 顺序无关；查找时建哈希表。三种存储类说明符/类型说明符都要覆盖，
        // 因为 C 的声明语法全靠它们区分声明与表达式（typedef 名字问题）。
        constexpr CKeyword kKeywords[] = {
            {"auto", CTokKind::KwAuto},
            {"break", CTokKind::KwBreak},
            {"case", CTokKind::KwCase},
            {"char", CTokKind::KwChar},
            {"const", CTokKind::KwConst},
            {"continue", CTokKind::KwContinue},
            {"default", CTokKind::KwDefault},
            {"do", CTokKind::KwDo},
            {"double", CTokKind::KwDouble},
            {"else", CTokKind::KwElse},
            {"enum", CTokKind::KwEnum},
            {"extern", CTokKind::KwExtern},
            {"float", CTokKind::KwFloat},
            {"for", CTokKind::KwFor},
            {"goto", CTokKind::KwGoto},
            {"if", CTokKind::KwIf},
            {"inline", CTokKind::KwInline},
            {"int", CTokKind::KwInt},
            {"long", CTokKind::KwLong},
            {"register", CTokKind::KwRegister},
            {"restrict", CTokKind::KwRestrict},
            {"return", CTokKind::KwReturn},
            {"short", CTokKind::KwShort},
            {"signed", CTokKind::KwSigned},
            {"sizeof", CTokKind::KwSizeof},
            {"static", CTokKind::KwStatic},
            {"struct", CTokKind::KwStruct},
            {"switch", CTokKind::KwSwitch},
            {"typedef", CTokKind::KwTypedef},
            {"union", CTokKind::KwUnion},
            {"unsigned", CTokKind::KwUnsigned},
            {"void", CTokKind::KwVoid},
            {"volatile", CTokKind::KwVolatile},
            {"while", CTokKind::KwWhile},
            // C11
            {"_Alignas", CTokKind::KwAlignas},
            {"_Alignof", CTokKind::KwAlignof},
            {"_Atomic", CTokKind::KwAtomic},
            {"_Bool", CTokKind::KwBool},
            {"_Complex", CTokKind::KwComplex},
            {"_Generic", CTokKind::KwGeneric},
            {"_Imaginary", CTokKind::KwImaginary},
            {"_Noreturn", CTokKind::KwNoreturn},
            {"_Static_assert", CTokKind::KwStaticAssert},
            {"_Thread_local", CTokKind::KwThreadLocal},
            // 常用系统头里的宏/关键字扩展（仅作高亮与识别，不参与 Sinter 语义）
            // 注意：`__attribute__` 故意**不**在这里映射成 KwInline —— 它是带参数的
            // 扩展说明符，映射成 inline 会让后面的 `((...))` 被当成表达式解析。
            // 词法器会把它整段吸收成 CTokKind::Attribute。
            {"__restrict", CTokKind::KwRestrict},
            {"__inline", CTokKind::KwInline},
            {"__volatile", CTokKind::KwVolatile},
            {"__const", CTokKind::KwConst},
            {"__bool_true_false_are_defined", CTokKind::KwBool},
        };
    } // namespace

    const CKeyword *c_keywords(size_t &count) {
        count = sizeof(kKeywords) / sizeof(kKeywords[0]);
        return kKeywords;
    }

    const char *c_tok_name(const CTokKind k) {
        switch (k) {
            case CTokKind::EndOfFile: return "EOF";
            case CTokKind::Unknown: return "<unknown>";
            case CTokKind::Identifier: return "identifier";
            case CTokKind::IntLiteral: return "integer";
            case CTokKind::FloatLiteral: return "float";
            case CTokKind::CharLiteral: return "char";
            case CTokKind::StringLiteral: return "string";
            case CTokKind::Attribute: return "attribute";
            case CTokKind::Hash: return "#";
            case CTokKind::HashHash: return "##";
            case CTokKind::PreprocLine: return "<preproc>";
            case CTokKind::KwAuto: return "auto";
            case CTokKind::KwBreak: return "break";
            case CTokKind::KwCase: return "case";
            case CTokKind::KwChar: return "char";
            case CTokKind::KwConst: return "const";
            case CTokKind::KwContinue: return "continue";
            case CTokKind::KwDefault: return "default";
            case CTokKind::KwDo: return "do";
            case CTokKind::KwDouble: return "double";
            case CTokKind::KwElse: return "else";
            case CTokKind::KwEnum: return "enum";
            case CTokKind::KwExtern: return "extern";
            case CTokKind::KwFloat: return "float";
            case CTokKind::KwFor: return "for";
            case CTokKind::KwGoto: return "goto";
            case CTokKind::KwIf: return "if";
            case CTokKind::KwInline: return "inline";
            case CTokKind::KwInt: return "int";
            case CTokKind::KwLong: return "long";
            case CTokKind::KwRegister: return "register";
            case CTokKind::KwRestrict: return "restrict";
            case CTokKind::KwReturn: return "return";
            case CTokKind::KwShort: return "short";
            case CTokKind::KwSigned: return "signed";
            case CTokKind::KwSizeof: return "sizeof";
            case CTokKind::KwStatic: return "static";
            case CTokKind::KwStruct: return "struct";
            case CTokKind::KwSwitch: return "switch";
            case CTokKind::KwTypedef: return "typedef";
            case CTokKind::KwUnion: return "union";
            case CTokKind::KwUnsigned: return "unsigned";
            case CTokKind::KwVoid: return "void";
            case CTokKind::KwVolatile: return "volatile";
            case CTokKind::KwWhile: return "while";
            case CTokKind::KwAlignas: return "_Alignas";
            case CTokKind::KwAlignof: return "_Alignof";
            case CTokKind::KwAtomic: return "_Atomic";
            case CTokKind::KwBool: return "_Bool";
            case CTokKind::KwComplex: return "_Complex";
            case CTokKind::KwGeneric: return "_Generic";
            case CTokKind::KwImaginary: return "_Imaginary";
            case CTokKind::KwNoreturn: return "_Noreturn";
            case CTokKind::KwStaticAssert: return "_Static_assert";
            case CTokKind::KwThreadLocal: return "_Thread_local";
            case CTokKind::LParen: return "(";
            case CTokKind::RParen: return ")";
            case CTokKind::LBrace: return "{";
            case CTokKind::RBrace: return "}";
            case CTokKind::LBracket: return "[";
            case CTokKind::RBracket: return "]";
            case CTokKind::Semicolon: return ";";
            case CTokKind::Comma: return ",";
            case CTokKind::Dot: return ".";
            case CTokKind::Arrow: return "->";
            case CTokKind::Ellipsis: return "...";
            case CTokKind::Plus: return "+";
            case CTokKind::Minus: return "-";
            case CTokKind::Star: return "*";
            case CTokKind::Slash: return "/";
            case CTokKind::Percent: return "%";
            case CTokKind::Amp: return "&";
            case CTokKind::Pipe: return "|";
            case CTokKind::Caret: return "^";
            case CTokKind::Tilde: return "~";
            case CTokKind::Bang: return "!";
            case CTokKind::Assign: return "=";
            case CTokKind::PlusAssign: return "+=";
            case CTokKind::MinusAssign: return "-=";
            case CTokKind::StarAssign: return "*=";
            case CTokKind::SlashAssign: return "/=";
            case CTokKind::PercentAssign: return "%=";
            case CTokKind::AmpAssign: return "&=";
            case CTokKind::PipeAssign: return "|=";
            case CTokKind::CaretAssign: return "^=";
            case CTokKind::ShlAssign: return "<<=";
            case CTokKind::ShrAssign: return ">>=";
            case CTokKind::EqualEqual: return "==";
            case CTokKind::NotEqual: return "!=";
            case CTokKind::Less: return "<";
            case CTokKind::Greater: return ">";
            case CTokKind::LessEqual: return "<=";
            case CTokKind::GreaterEqual: return ">=";
            case CTokKind::AmpAmp: return "&&";
            case CTokKind::PipePipe: return "||";
            case CTokKind::Shl: return "<<";
            case CTokKind::Shr: return ">>";
            case CTokKind::PlusPlus: return "++";
            case CTokKind::MinusMinus: return "--";
            case CTokKind::Question: return "?";
            case CTokKind::Colon: return ":";
            case CTokKind::Comment: return "<comment>";
        }
        return "<unhandled>";
    }
} // namespace sinter::cfront