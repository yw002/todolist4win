#include <string>
#include <vector>

#include "domain/BodyDocument.h"
#include "domain/Json.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {
void Check(const char* name, bool value, const char* detail = "") { ReportTest("BodyDocument", name, value, detail); }

std::string Key(char fill) { return std::string(64, fill); }

BodyBlock ImageBlock(const std::string& key) {
    BodyBlock block;
    block.kind = BodyBlockKind::Image;
    block.image.key = key;
    block.image.mime = "image/png";
    block.image.width = 1280;
    block.image.height = 720;
    return block;
}
}  // namespace

void RunBodyDocumentTests() {
    // --- Minimal strict JSON codec ------------------------------------------
    auto scalar = json::Parse("{\"a\":1,\"b\":[true,null,\"x\\ny\"],\"c\":{\"d\":-2.5e3}}");
    Check("json object parses", scalar.ok());
    Check("json object is compact on dump",
          scalar.ok() && json::Dump(scalar.value()) == "{\"a\":1,\"b\":[true,null,\"x\\ny\"],\"c\":{\"d\":-2.5e3}}");
    Check("json Find reaches nested value", scalar.ok() && scalar.value().FindString("a") == nullptr);
    Check("json preserves number literal", scalar.ok() && scalar.value().Find("a") != nullptr &&
                                           scalar.value().Find("a")->numberText == "1");
    Check("json preserves raw slice", scalar.ok() && !scalar.value().raw.empty() && scalar.value().raw.front() == '{');
    Check("json decodes \\n escape", scalar.ok() && scalar.value().Find("b") != nullptr &&
                                     scalar.value().Find("b")->elements[2].stringValue == "x\ny");
    Check("json decodes surrogate pair", json::Parse("\"\\uD83D\\uDE00\"").ok() &&
                                        json::Parse("\"\\uD83D\\uDE00\"").value().stringValue == "\xF0\x9F\x98\x80");
    Check("json rejects lone high surrogate", !json::Parse("\"\\uD83D\"").ok());
    Check("json rejects lone low surrogate", !json::Parse("\"\\uDC00\"").ok());
    Check("json rejects trailing garbage", !json::Parse("{} {}").ok());
    Check("json rejects unquoted key", !json::Parse("{a:1}").ok());
    Check("json rejects leading zero", !json::Parse("01").ok());
    Check("json rejects raw control char", !json::Parse(std::string("\"a\tb\"")).ok());
    Check("json rejects invalid utf8", !json::Parse(std::string("\"\xC0\xAF\"")).ok());
    Check("json rejects overlong utf8", !json::IsValidUtf8(std::string("\xE0\x80\xAF")));
    Check("json accepts CJK utf8", json::IsValidUtf8("中文〔图片〕"));
    Check("json Quote escapes control bytes", json::Quote("a\tb\"c\\d") == "\"a\\tb\\\"c\\\\d\"");
    json::Limits small;
    small.maxArrayElements = 2;
    Check("json enforces array element limit", !json::Parse("[1,2,3]", small).ok());
    json::Limits shallow;
    shallow.maxDepth = 2;
    Check("json enforces depth limit", !json::Parse("[[[0]]]", shallow).ok());

    // --- Legacy / empty documents -------------------------------------------
    BodyDocument legacy = BodyDocument::FromLegacyNote("第一行\n第二行");
    Check("legacy has one text block", legacy.blocks.size() == 1 && legacy.blocks[0].kind == BodyBlockKind::Text);
    Check("legacy is flagged", legacy.legacy);
    Check("legacy projection equals note", legacy.PlainText(false) == "第一行\n第二行");
    Check("legacy json round trips", BodyDocument::Parse(legacy.ToJson()).ok());
    Check("empty has one empty text block", BodyDocument::Empty().blocks.size() == 1 &&
                                           BodyDocument::Empty().blocks[0].text.empty());
    Check("empty is not legacy", !BodyDocument::Empty().legacy);
    Check("empty validation succeeds", BodyDocument::Empty().Validate().ok());

    // --- Text / image / unknown blocks --------------------------------------
    BodyDocument document = BodyDocument::Empty();
    document.blocks[0].text = "先做调研\n再写代码";
    const std::string firstKey = Key('a');
    const std::string secondKey = Key('b');
    document.blocks.push_back(ImageBlock(firstKey));
    BodyBlock trailing;
    trailing.kind = BodyBlockKind::Text;
    trailing.text = "以上是截图";
    document.blocks.push_back(std::move(trailing));
    const std::string encoded = document.ToJson();
    Check("toJson is compact", encoded.find('\n') == std::string::npos && encoded.find(" ") == std::string::npos);
    Check("toJson has fixed key order",
          encoded.find("{\"v\":1,\"blocks\":[{\"t\":\"text\"") == 0);
    auto reparsed = BodyDocument::Parse(encoded);
    Check("json round trip", reparsed.ok());
    Check("round trip keeps block count", reparsed.ok() && reparsed.value().blocks.size() == 3);
    Check("round trip keeps text", reparsed.ok() && reparsed.value().blocks[0].text == "先做调研\n再写代码");
    Check("round trip keeps image dimensions", reparsed.ok() && reparsed.value().blocks[1].image.width == 1280 &&
                                               reparsed.value().blocks[1].image.height == 720);
    Check("projection excludes image blocks", document.PlainText(false) == "先做调研\n再写代码以上是截图");
    Check("history projection includes placeholder",
          document.PlainText(true) == "先做调研\n再写代码" + std::string(kImagePlaceholder) + "以上是截图");
    Check("image keys preserve order", document.ImageKeys() == std::vector<std::string>{firstKey});

    document.blocks.push_back(ImageBlock(firstKey));
    document.blocks.push_back(ImageBlock(secondKey));
    Check("image keys de-duplicate", document.ImageKeys() == (std::vector<std::string>{firstKey, secondKey}));
    Check("document validates", document.Validate().ok());

    // --- Unknown blocks and unknown members survive a v1 save ---------------
    const std::string futureJson =
        "{\"v\":1,\"blocks\":["
        "{\"t\":\"text\",\"text\":\"hello\",\"marks\":[{\"type\":\"bold\",\"from\":0,\"to\":5}],\"style\":\"heading\"},"
        "{\"t\":\"table\",\"rows\":[[1,2],[3,4]],\"caption\":\"future\"},"
        "{\"t\":\"image\",\"key\":\"" + firstKey + "\",\"mime\":\"image/webp\",\"w\":10,\"h\":10,\"animated\":true}"
        "]}";
    auto future = BodyDocument::Parse(futureJson);
    Check("future-flavoured v1 document parses", future.ok());
    Check("unknown block becomes Unknown", future.ok() && future.value().blocks[1].kind == BodyBlockKind::Unknown);
    Check("unknown block keeps verbatim json", future.ok() &&
                                              future.value().blocks[1].rawJson.find("\"rows\"") != std::string::npos);
    Check("unknown members are preserved", future.ok() &&
                                           future.value().blocks[0].rawJson.find("\"marks\"") != std::string::npos &&
                                           future.value().blocks[0].rawJson.find("\"style\"") != std::string::npos);
    Check("mime is not hardcoded to png", future.ok() && future.value().blocks[2].image.mime == "image/webp");
    const std::string rewritten = future.ok() ? future.value().ToJson() : std::string();
    Check("reserialised doc still contains marks", rewritten.find("\"marks\"") != std::string::npos);
    Check("reserialised doc still contains table", rewritten.find("\"t\":\"table\"") != std::string::npos);
    Check("reserialised doc still contains animated", rewritten.find("\"animated\":true") != std::string::npos);
    Check("reserialised doc is valid json", json::Parse(rewritten).ok());
    auto twice = BodyDocument::Parse(rewritten);
    Check("unknown fields survive two round trips", twice.ok() &&
                                                     twice.value().blocks[0].rawJson.find("\"marks\"") != std::string::npos &&
                                                     twice.value().blocks[1].rawJson.find("\"rows\"") != std::string::npos);
    Check("future-flavoured document still validates", future.ok() && future.value().Validate().ok());

    // --- Version gate --------------------------------------------------------
    auto newer = BodyDocument::Parse("{\"v\":2,\"blocks\":[{\"t\":\"text\",\"text\":\"v2 only\"}]}");
    Check("v>1 is refused", !newer.ok());
    Check("v>1 maps to BodySchemaUnsupported", newer.error().code == ErrorCode::BodySchemaUnsupported);
    auto corrupt = BodyDocument::Parse("{\"v\":99,\"blocks\":[not json");
    Check("corrupt document is refused", !corrupt.ok());
    Check("missing v is refused", !BodyDocument::Parse("{\"blocks\":[]}").ok());
    Check("missing blocks is refused", !BodyDocument::Parse("{\"v\":1}").ok());
    Check("wrong root type is refused", !BodyDocument::Parse("[]").ok());
    Check("empty document string is refused", !BodyDocument::Parse("").ok());
    BodyDocument badVersion = BodyDocument::Empty();
    badVersion.version = 2;
    Check("Validate rejects v>1", badVersion.Validate().error().code == ErrorCode::BodySchemaUnsupported);

    // --- Limits --------------------------------------------------------------
    BodyDocument tooMany = BodyDocument::Empty();
    for (std::size_t i = 0; i < kMaxBlocks; ++i) {
        BodyBlock block;
        block.text = "x";
        tooMany.blocks.push_back(std::move(block));
    }
    Check("over block limit refused by Parse", !BodyDocument::Parse(tooMany.ToJson()).ok());
    Check("over block limit refused by Validate", tooMany.Validate().error().code == ErrorCode::BodyTooLarge);
    BodyDocument huge = BodyDocument::Empty();
    huge.blocks[0].text = std::string(kMaxBodyDocBytes, 'x');
    Check("over 256 KiB refused by Validate", huge.Validate().error().code == ErrorCode::BodyTooLarge);
    Check("over 256 KiB refused by Parse", !BodyDocument::Parse(huge.ToJson()).ok());

    BodyDocument badKey = BodyDocument::Empty();
    badKey.blocks.push_back(ImageBlock("ABC"));
    Check("short key refused", badKey.Validate().error().code == ErrorCode::ImageInvalid);
    BodyDocument upperKey = BodyDocument::Empty();
    upperKey.blocks.push_back(ImageBlock(std::string(64, 'A')));
    Check("uppercase key refused", upperKey.Validate().error().code == ErrorCode::ImageInvalid);
    BodyDocument badDims = BodyDocument::Empty();
    BodyBlock wide = ImageBlock(firstKey);
    wide.image.width = kMaxImageDimension + 1;
    badDims.blocks.push_back(wide);
    Check("over 8192 width refused", badDims.Validate().error().code == ErrorCode::ImageTooLarge);
    BodyDocument bomb = BodyDocument::Empty();
    BodyBlock bombBlock = ImageBlock(firstKey);
    bombBlock.image.width = 8192;
    bombBlock.image.height = 8192;
    bomb.blocks.push_back(bombBlock);
    Check("over 32 M pixels refused", bomb.Validate().error().code == ErrorCode::ImageTooLarge);
    BodyDocument tooManyImages = BodyDocument::Empty();
    for (std::size_t i = 0; i <= kMaxImagesPerTask; ++i) {
        tooManyImages.blocks.push_back(ImageBlock(std::string(64, static_cast<char>('a' + i % 6))));
    }
    Check("over 20 images refused", tooManyImages.Validate().error().code == ErrorCode::BodyTooLarge);
    BodyDocument tooLongNote = BodyDocument::Empty();
    tooLongNote.blocks[0].text = std::string(60001, 'x');
    Check("projection still honours ValidateNote", !tooLongNote.Validate().ok());
    BodyDocument brokenFragment = BodyDocument::Empty();
    brokenFragment.blocks[0].rawJson = "\"oops\":";
    Check("malformed preserved fragment refused", !brokenFragment.Validate().ok());
}
