#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/executor.hpp"
#include "tal/pixels.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

#include "gifting.hpp"

// GIF: our own decoder in tal-media, through `tal::media::decoded` like any caller, and directly
// where only the decoder can be asked.
//
// The sample files are kept here as bytes, not as a description, and that is deliberate: they were
// written by someone else's encoder, not ours. As a description, a trial would check our decoder
// against our own encoder, which proves nothing. Pixel-exact agreement with an independent decoder
// and with a browser was checked when the decoder was written; here we make sure that what was
// once matched does not move.

namespace {

using tal::Bytes;
using tal::media::Pixels;
using tal::media::Size;
using tal::media::Still;

// Bytes from a string of lower-case hexadecimal digits, the form in which the samples are kept.
Bytes lump(std::string_view hex) {
    Bytes made;

    made.reserve(hex.size() / 2);

    const auto value = [](char one) { return one >= '0' && one <= '9' ? one - '0' : one - 'a' + 10; };

    for (std::size_t at = 0; at + 1 < hex.size(); at += 2)
        made.push_back(static_cast<std::byte>(value(hex[at]) * 16 + value(hex[at + 1])));

    return made;
}

struct Tone {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;

    bool operator==(const Tone&) const = default;
};

// The color of pixel (`x`, `y`) in frame number `still` of `one`.
Tone at(const Pixels& one, std::uint32_t x, std::uint32_t y, std::size_t still = 0) {
    const std::size_t where = (std::size_t{y} * one.size().width + x) * 4;
    const Bytes& bytes = one.stills()[still].rgba;

    return {std::to_integer<std::uint8_t>(bytes[where + 0]), std::to_integer<std::uint8_t>(bytes[where + 1]),
            std::to_integer<std::uint8_t>(bytes[where + 2]), std::to_integer<std::uint8_t>(bytes[where + 3])};
}

// The image `given` decoded whole, at most `most` frames (all when zero), with no adapter.
Pixels pixels(const Bytes& given, std::size_t most = 0) {
    tal::Loop loop;

    return tal::wait(loop, tal::media::decoded(given, Size{}, tal::Optional<tal::media::Decoding>{}, most));
}

// What decoding `given` threw: its kind and message, or "none".
std::string refusal(const Bytes& given) {
    try {
        (void)pixels(given);
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())} + ": " + why.what();
    }

    return "none";
}

// The delay of frame number `still` of `one`, in milliseconds.
long long after(const Pixels& one, std::size_t still) { return one.stills()[still].after.count(); }

constexpr std::string_view plain =
    "47494638376107000500810000000000ff000000ff000000ff2c0000000007000500000816000104103040a0"
    "8081051112345830e1c2000c0d0604003b";

constexpr std::string_view flat =
    "4749463839610b000900810000000000ff000000ff000000ff2c000000000b000900000213848c7731ecdd54"
    "4af0bd2957dd2163be615301003b";

constexpr std::string_view woven =
    "4749463839610b000900810000000000ff000000ff000000ff2c000000000b000900400212848c77c9ab0d9f"
    "902d0c8bafce7cfb0e0e05003b";

constexpr std::string_view clear =
    "47494638396107000500810000000000ff000000ff000000ff21f90401000000002c00000000070005000008"
    "16000104103040a08081051112345830e1c2000c0d0604003b";

constexpr std::string_view dot =
    "4749463837610100010081000009c81e0000000000000000002c000000000100010000080400010404003b";

constexpr std::string_view leaving =
    "47494638396110000c00810000000000ff000000000000000021ff0b4e45545343415045322e300301000000"
    "21f90404040000002c0000000010000c000008220003080c00a0a0c18305071244c85021c386031f4a9c48b1"
    "a2c58b18336adc2831200021f90405000002002c00000000070006008100000000ff00000000000000081900"
    "01080420a0e0408206050658186020c30005053c8cf830200021f90405010002002c03000200070006008100"
    "00000000ff00000000000008190001080420a0e0408206050658186020c30005053c8cf830200021f904050c"
    "0002002c060004000700060081000000ff000000000000000008190001080420a0e0408206050658186020c3"
    "0005053c8cf83020003b";

constexpr std::string_view clearing =
    "47494638396110000c00810000000000ff000000000000000021ff0b4e45545343415045322e300301000000"
    "21f90409060000002c0000000010000c000008220003080c00a0a0c18305071244c85021c386031f4a9c48b1"
    "a2c58b18336adc2831200021f90409060000002c03000200040004008100000000ff00000000000000080900"
    "03081c48b06080800021f90409060000002c0600040004000400810000000000ff0000000000000809000308"
    "1c48b06080800021f90409060000002c090006000400040081000000ff000000000000000008090003081c48"
    "b0608080003b";

constexpr std::string_view backing =
    "47494638396110000c00810000000000ff000000000000000021ff0b4e45545343415045322e300301000000"
    "21f9040c060000002c0000000010000c000008220003080c00a0a0c18305071244c85021c386031f4a9c48b1"
    "a2c58b18336adc2831200021f9040d060002002c00000000070006008100000000ff00000000000000081900"
    "01080420a0e0408206050658186020c30005053c8cf830200021f9040d060002002c03000200070006008100"
    "00000000ff00000000000008190001080420a0e0408206050658186020c30005053c8cf830200021f9040d06"
    "0002002c060004000700060081000000ff000000000000000008190001080420a0e0408206050658186020c3"
    "0005053c8cf83020003b";

// A whole real file, byte for byte. The other tests check single mechanisms on files chosen so that
// they can be read; this one is here so that nothing in the decoder moves unnoticed. It was written
// by someone else's encoder and contains everything at once:
//
//   sixteen images, fifteen of them with a local palette;
//   all three disposal methods: leave, to background, restore previous;
//   fifteen different rectangle sizes, because the encoder crops each frame to the changed area;
//   a transparent index;
//   zero and shortest delays, that is the clamp to a hundred milliseconds.
//
// Two kilobytes in the test source. They are worth it.
constexpr std::string_view proof =
    "4749463839613000200081000000000025477100000000000021ff0b4e45545343415045322e30030100000021f90409"
    "040000002c00000000300020000008780001081c48b0a0c18308132a5cc8b0e1c100102346744851a1c48b15331abc28"
    "51a347811c277ed41812e24892254f662c1940a5cb970331c274c872e64296266d3ec4995327419e3d7d02002a5228d1"
    "a03a8fb6143af428d3a6449f42c529756acdaa3cabc64ca9f567c7ae60c38a1d4bb6acd9b31e03020021f9040d070000"
    "002c020003002c001900810000004a8ee228388800000008700005081c381080c18308132a5cb890a043861023327448"
    "50a2458b140b5edcd830a3008e20117afc1832e4c8922853aa5cc9b2a5cb970703c8940933e2cc9b346b26c4c953674c"
    "9e3d7d020d5a7328509d4689be4c8a1329d3994e9f0610fad42700a956af26cd6ad02857844d1106040021f904050a00"
    "00002c020003002c001c00810000006fd5532b299f00000008720001081c48b0a0c18308110a58c89061c287100b369c"
    "18b1e2c3890d2d6a3488d1e1c68f003a2e04f951a400921b4da25cc9b2a5cb973063ca9c491362809b376b1ac4c933a7"
    "4e003d83ea0c4ab42651a1338f1695a91469cca63d9342c529756a009a568d4e1ddaf427d0a35e07463d18100021f904"
    "09000000002c0200040029001c0081000000941cc42e1ab6000000086c0001081c48b0a0c183080b0a58c89061c28710"
    "05369c18b1e2c1890d2d6a1c88d1e1468d1d177e041972a4c590024caa5cc9b2a5cb973063ca9c49f361809b376b0ec4"
    "c93327cd9e4067021d2a736850984689be4c7ad425d39e489fe28c2a3540ccaa45a50a655a3329c2800021f9040d1000"
    "00002c020006001e001a0081000000b96335310bcd00000008600005081c381080c18308132a24c850a1c3870c093e9c"
    "883062418a142d0ac4985123c7891a057c1c49b2a4c9932853aa5cc9926580972f5dc29c192025cd9b276fea34a91327"
    "c99e3b4702f5f97128cd9f4661224d5a73e9509e49733e0510100021f90405010000002c020006001e001a0081000000"
    "b96335deaaa600000008620001081c48b0a0c1830203285cb810a143820c233e9c1891e1c487151b5e449851e1468e1d"
    "3f1eec1840a4c9932853aa5cc9d2a080972f5bc29c193325cd9b286fea3ca913a7c89e3b3f02f5b97128cd9f4661224d"
    "2ac024539e49730e5509f461400021f90409070000002c0200020017001e008100000003f117bc544c00000008620005"
    "081c381080c1830811125c98b0e1c185041d3a84585062428a022d5ec4a85121c78e20438a1c49b2a4c9932853360cc0"
    "9225c996305d828c49b323cd9b1a6fd694a813a7c39e3b570285c97368cba24603584c9ad3a84da0217bbe24aa526340"
    "0021f9040d0a0000002c020003000f00190081000000283888bf4563000000084e0005081c381080c18304131e4498b0"
    "e042000d1d2e8c28f021448a16290ab0c8b1a3c78f201f061839b223c9932517a25c7970a54b832e59c69c49b3a6cd9b"
    "3601dc849993e74c91314d9e7c18100021f904050d0000002c0100030010001900810000004d7ff9c2367a0000000851"
    "0001081c48b0a04001081326340840a143860e15428c887022458b11313a0c10c020c78f2039120c4952a4c092244fa2"
    "4cb992654b90005ec294f951e5cb813447b6f48892a14d933e830a1d4ab468c1800021f90409100000002c0100040010"
    "001400810000004a8ee272c66a000000084d0001081c28a060c1810303285cb8d0204286100d0a4808b1a14481152d3a"
    "049051a1c4831d037c9c1872248091280fa6442970e5c5962e0f1274891026cb9a3337e2dcc9b3a7cf9f4083ee0c0800"
    "21f9040d040000002c0200010012000f00810000004d7ff9970ddb00000008420001081c28508041830413163cc85080"
    "c2850d192a8c483121c588000268dcb8f1a2008e203d82e42872a4c69226518ebc98d1640096163d3a7ce8f121c18609"
    "03020021f90405070000002c020000001700100081000000bc544c50701000000008500001081c4870608083070b2a2c"
    "88b061c2850c1d3a846850a2448a002c5e84a8d122c78e0e05881c3912644392284d224449526500002c4baa84195340"
    "4690026bdabce931674d8213310a1d4a9460400021f904090a0000002c020000001d000e0081000000e19bbd53612700"
    "000008510001081c48b0a0c00008132634c8d0a0c28708050a984891a24388100154dc7811a3428d1b2d12f48831a4c8"
    "8124219a9c5830e5c395025aba4c08b363ca860d6706c099f326cf9e187ffe7c28b428ce800021f9040d0d0000002c02"
    "00000024000e008100000006e22e56523e00000008560001081c48b0a0c1820112263cc8b02142851003389cf8302244"
    "8102326ad4481180c58f00368aecf8d16248911c2796fc8832a5c395165b66a40833a24c01346b2abc495267c7973e7f"
    "36ac295465c9a23d15160c080021f90405100000002c020000002900100081000000deaaa62b299f0000000860000108"
    "1c48b0a0c18308132a5c68508043870c23267c4811a2c48b002a6a1418a0a3478f18056a1c09e0a3c9901947562c6912"
    "244695235bbabc0853a3cc8e216b56bc1920a7ce873c7dfe148072e2cfa24675224daa72a942984e19ae2418100021f9"
    "0409040000002c020001002c00120081000000e19bbd50701000000008660001081c48b0a0c18308132a5cc8b0a1c387"
    "10233614409162c1001833669458b0a2478b02358ae438f0a3c99022379234c912404a951c599e7c8971a54c933403d8"
    "bce931e74e9e20491e04ea512842a2158d1e25aa7429d0a64e65424d7873eac28f050302003b";

// Noise from Pillow's encoder: 64 by 64 pixels of sixteen random colors, which fill the table of
// codes past 2048 entries, so the codes grow to twelve bits. Its fingerprint is that of Pillow's
// own decoding, an independent decoder.
constexpr std::string_view noise =
    "4749463837614000400083000029bcd2b22b8e6e9a294f7424997ff91f2cd6723e0d674a49470cd012bff33186f7c3c9"
    "3653f265ed4035e274b1781cf22c00000000400040004008ff00093c7020c04103000a0c1c08202081810102162c18c0"
    "20c003010a08687c508081800202002068d08081820703010038804001028e08063c0880000183040108086840c08182"
    "000706f434a09300510105152800e9c0c101050d0a38185040010006083a1a1888e000cc030406207040734183070bb6"
    "66bccaa040010300022860b0808082900b182a709ae02ad0960f202e98e9b266db010d0624542af5e702005245660cd0"
    "f78182a705341aa878990003820f54c6edb8602483b30b8d526c7077abc5070812603c0040e8019c05145365e0d1a781"
    "880f0ce08c8a60c167072d053c784a0000ec052c4f232858bc4080053bf706ee6b106d81d0087a56ff75cab3416ab40f"
    "1260fd6ec0a54bb2cd132c7000607ed601b29fdead8958acfcc4093420dc7508181040033139b05960b70d00006b0114"
    "00c043059af75950e97946d4016619509501366d6691730008405453df2dc71b003e211600412381145b810e68241341"
    "cee554d26b0ea8171e43030ce0006f073c55d34b083590c0019965f5a2020314e9c0770c2540904b7215009d62025834"
    "1f520910e0d067c25d16de4176193800432f2576db77050d2600010844281382e609b8800275bd1811760c0090006c32"
    "8977dd445a1ac75302093945134b350ee6914c225d75166f01e077555d05f465a0921809b9df778c72e42142073c14d7"
    "750d28d7635a0c04ff07db4871c5ba5000711d605e50015a0755805011601d7e3cb9b55b716d31d09371110e04dba094"
    "a52a5b029d6e46547d3f09559b591805d8148711f6d80072613527d56607f066638f6bd66897a0c389459f909b89d4d2"
    "48029c469fa066b525d547afb5a7a487039436d34ea5418495512a5106535b36e94a4069094565e9565119d05e575b09"
    "501c640b0d96f0c0f902aaec660547655741565ec661012409095d48baea1a5e7d66912540507bc6559b462da9f4d645"
    "741ad0aa7c09a817915d6891c540940c1c8011740f20e85673b10ebdf34ab7addc656db58d5416999c052a64ab049537"
    "1575e11184556983be94ae03cb39d0e19c7682e5a143a17affcc51444c4ee9f1839585b7d3418f4555d24ea1c904336d"
    "7485061d885915c9f744222de4109dc6c56ad07c8847591aa2043cc512cc4e2d7591779bed0951730b4da853d5742a57"
    "15007621d651814fdd94d549b9c9c552c15b81c822022b511b9bae4e095ea2737c3d0d62dc028a641d4bb70a40e98342"
    "9104164f353e855645d47204595b43e21aa6401e4b98d66feaa1355f5f812aa5d455626a291f74bbc275007dc8db4da6"
    "1c0423bf84062a35420a9fe6e4118420c43334d9cc77c83213ebe9ac44ff0b80e70a66b7c760652e80338ad482b226fc"
    "5c667336290093a81200ca94ef2101920842c8451609c1cc2206496103f614a80558073a60618042ff1ee4922eed4923"
    "0b48c074d2759b003ce427da53d89296c2ad89d82d50f4795545e846005c114558ad1a8b135d92aaabc04c4e13ba49aa"
    "e0739567e10e413bd30d6da4f616f998a44433698a068bb39db128482e0fc0ca834a13ab2095e4263ff94d7dd61435cf"
    "08642ad381536e9a531f2742c629ddaa49ce0e22a8223d40270efa6462c2731c0e75a54461d15e4568839301fc462809"
    "09d422db6514d9a465661212e4ea38439b1269e9971c32154b5ca5c1b32de72353514a4e4c4291205de7343933804462"
    "52359204503793dc8b583e1994b83484431ad94b4660631efc3407455312d28478921eaf3c049db1ba8b581217959950"
    "643074cb4d903052ff20273ec62a9eb95e7ba8322e1391a52948f192522ce4215c090a242e71654b9498c7bdf0e6240d"
    "318847c22444017dc728a5f3905210d323380507442571d94c82a39392c0c6400d399082b26290661dae3d6aa4162535"
    "42993a15a84e53825292c8628029296e62a01c89ccbe831d1f2d852a3d122abe02d595e9c0252d820a9298a612965805"
    "e89e4b29d25546d2c5042846243949c858c202c300a9c5250dc0d56712b2332cba84a36389cba0a8c292a2b60a4af7aa"
    "eb4934b6a6cba0e5251d41098ba2c42f1081c841e96a8f086173a07145a522a7e9d4532ca2bdb348e42e66d913269f82"
    "90ad38c7a70e0a1b493cdb29cb20256d9d9288d144e894a8ff4588243ae9cbcd1c942f8528315043db6aad9413c80c85"
    "2f22cc841222bb08beb628803b3d21d7caf42ab454b5052882aa495117922ab909f13613a2d63cd5b99721bd2426fff3"
    "4b6eea62969194a62942a44f8010f43fdfb8e53a7095cd4858d2958a14362277828e4032a59129dd24345df2085cf8d4"
    "928958443120a1ce4de29a2f9e4c88bf19618d35cfc7a8ac2ccf28c6b3dd527cc2138a444d59d3698b0f23c2a7245a09"
    "bf200a0e6a4c441ba418c82139c14e84d0cb135da9ce68082208e3cac82213d1057831346b484eb3248f84a400d3a99b"
    "59a10219e365642114f9df730952a45679082d08b2cc9a4c549209e1ea2c56dad945f67390b7e4c4ff443d696143f622"
    "cd1a758400026a89f1c2641baacc8425cba94f7ac6c7c79e3cb7392fca8c5776fc1da868294a97a10f48e462a5b47c52"
    "2213e1cd6f2282933aa1c4241f998bdd867511e4b457294f8b1a7d34462d261d1ac18282585a4ef9268e3c6a8ac2718e"
    "0ad3354d9d74d6335641495d8a6a2026c1ae23b9b1b2a00a2224ec9d44ce41f9d07582d412b74c882f4159d94506a312"
    "d92c67db9dca4cc7ba6790ac82ec32f1e5f3b8906337cf20272a21f9c84da83524932ce493ba93d519711794027d4628"
    "0a7a8a5958e4162df5042dd2fca5f6dc822784b3a656cacadc56f2d2a5d78685a03371a2c62833911375a9242e928b81"
    "661b9298646427fcff068944740a6582bcc83c29a6885c5c5222ac749451b1aec875f6351d3f2e25530629a313bd859d"
    "397908801cb90d4732bc95a9a0ee7f50a7db45289346e42904250211a4824a03b340996e622f79da4162b5a7164e67d8"
    "d889eb606cb35d4a69ce2b3e0c12dc1e8a923ad5c727f5d172b84436b1f9a0e5ccd14db392ef42d5ac4a873503898a0f"
    "eb1312b1e8e73b9f99937280221cf65aa9236a95da9c82a663a780a56ac8554cba285235b1ec0c4a0ed64b5c9ba61280"
    "af4a21498c2b51e4632009958e511cda61e98422ac257dce325bd4a99842722dd0fd0fc4b59450abbac42794b42a53d7"
    "41c9434669349b1386263b44ce928cd2d3f6d0a69dafa58cff0abfe992f1a44a421cda3cca90c7229ef049500fe6dfbe"
    "167243a985655c32c94894666234b9e04e834a32754b32562c92298cf3336bc22881a246b0e148318167ca5230a1251c"
    "a73227661548dee620389144a62421bfa11046c71a05416cc4332c31d11028353a8c825c3db5174301193d311636f111"
    "28b13f3a4126761324838234236534ff5311b0d125b60165dca642505630b5b115492744a505112c32110ab826b84311"
    "56c22e74d12a82a24c02a12453a425e9f213efa71e42d4131051355ee110812621bd521442011b649141112112ac3117"
    "c3e3756de221bca1443ad510c51115df111b88111c0912281ae321da932f1fa13d25921673d1295079e219553640fea3"
    "4874483148b127c8231045f5494001117536721ad71776916270a12071f534a51316ff631d68412f8c631163a14279e1"
    "1bb58143c2d216965135cd2328ad921b20811e1d3127cea21c182421b8a21364a13c03f11146612200a5688af1197368"
    "565f958768f51169018b39b4106782120101003b";

// What no encoder writes, made by hand for the damage it shows, each with a palette of black, red,
// green and blue and a code size of two (clear 4, end 5, the first new code 6), unless it says
// otherwise. Where Pillow reads the file, it reads it as we do; where it refuses, the trial says
// what we make of it.
//
// A clear in the middle, after a code was added: 4 1 2 4 3 3 6 5 is red, green, blue, blue, blue,
// blue (6 is blue twice after the clear, not red and green as before it).
constexpr std::string_view cleared =
    "47494638396106000100810000000000ff000000ff000000ff2c00000000060001000002048cb8b900003b";

// No clear at the start, which the standard allows: 1 2 6 5 is red, green, red, green.
constexpr std::string_view bare =
    "47494638396104000100810000000000ff000000ff000000ff2c0000000004000100000202910b003b";

// A code ahead of the table: 4 1 7 5 ends after red, since 7 is not yet a code.
constexpr std::string_view ahead =
    "47494638396102000100810000000000ff000000ff000000ff2c0000000002000100000202cc0b003b";

// Data after the end code: 4 1 5 2, where the green after the end is not a pixel.
constexpr std::string_view stopped =
    "47494638396102000100810000000000ff000000ff000000ff2c00000000020001000002024c05003b";

// A code size of one, below the two the standard allows: 2 1 3.
constexpr std::string_view narrow =
    "47494638396102000100810000000000ff000000ff000000ff2c000000000200010000010136003b";

// A palette of two, black and red, and the index 3 beyond it: 4 0 3 5.
constexpr std::string_view beyond =
    "47494638396102000100800000000000ff00002c0000000002000100000202c40a003b";

// A 4 by 4 canvas: a red frame of 4 by 4 at (2, 2), reaching past the canvas, cleared to the
// background, then a green pixel at (0, 0).
constexpr std::string_view outside =
    "47494638396104000400810000000000ff000000ff000000ff21f904080a0000002c020002000400040000020a4c1211"
    "1111114208a100002c00000000010001000002025401003b";

// A red frame with the reserved disposal 6, then a green pixel at (0, 0).
constexpr std::string_view reserved =
    "47494638396102000100810000000000ff000000ff000000ff21f904180a0000002c00000000020001000002024c0a00"
    "2c00000000010001000002025401003b";

// A comment of four bytes that would read as a delay of half a second, then red and green.
constexpr std::string_view commented =
    "47494638396102000100810000000000ff000000ff000000ff21fe0400320000002c00000000020001000002028c0a00"
    "3b";

// An unknown byte (0xAB) before the image, which Pillow skips over.
constexpr std::string_view lost =
    "47494638396102000100810000000000ff000000ff000000ffab2c00000000020001000002028c0a003b";

// A frame whose data ends at once (4 5), then red and green.
constexpr std::string_view silent =
    "47494638396102000100810000000000ff000000ff000000ff2c00000000020001000002012c002c0000000002000100"
    "0002028c0a003b";

// A frame of half a second, then one without control: the control holds for one frame only.
constexpr std::string_view reset =
    "47494638396102000100810000000000ff000000ff000000ff21f90400320000002c00000000020001000002028c0a00"
    "2c0000000002000100000202540a003b";
}

// --- Refusals from the header --------------------------------------------------------------------

// The decoder asked directly refuses what is not a GIF; through `decoded` a JPEG never reaches it,
// and without an adapter for JPEG it is unsupported.
TRIAL("GIF: a foreign format is malformed for the decoder and unsupported without an adapter") {
    std::string said = "none";

    try {
        tal::media::gifting::gifted(lump("ffd8ffe000104a464946000102000000"), [](auto, auto, auto) { return true; });
    } catch (const tal::Trouble& why) {
        said = std::string{tal::spoken(why.kind())} + ": " + why.what();
    }

    SAME(said, std::string{"malformed: Not a GIF."});
    SAME(refusal(lump("ffd8ffe000104a464946000102000000")), std::string{"unsupported: No decoder of JPEG."});
    SAME(refusal(lump("0102030405")), std::string{"unsupported: Not an image tal knows."});
}

// --- Still image -----------------------------------------------------------------------------

TRIAL("GIF: a single pixel of a single color") {
    const Pixels one = pixels(lump(dot));

    OUGHT(one.held());
    SAME(one.size().width, 1U);
    SAME(one.size().height, 1U);
    SAME(one.stills().size(), std::size_t{1});
    OUGHT(at(one, 0, 0) == (Tone{9, 200, 30, 255}));
}

// The picture is a pattern: the color of a pixel is (x + y) % 4 from the palette black, red, green,
// blue. It is checked by the pattern rather than by a written-out expectation, because a mistake in
// copying thirty-five pixels would be indistinguishable from a mistake in the decoder.
TRIAL("GIF: palette and pixel order") {
    const Pixels one = pixels(lump(plain));
    constexpr Tone palette[4] = {{0, 0, 0, 255}, {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};

    SAME(one.size().width, 7U);
    SAME(one.size().height, 5U);

    bool held = true;

    for (std::uint32_t y = 0; y < 5; ++y)
        for (std::uint32_t x = 0; x < 7; ++x) held = held && at(one, x, y) == palette[(x + y) % 4];

    OUGHT(held);
}

// The same picture written in two ways must come out the same. Interlacing arranges the rows in
// four passes (0 every 8, 4 every 8, 2 every 4, 1 every 2), and that is the easiest place to
// stumble, because the data comes in order but the rows do not.
TRIAL("GIF: interlacing gives the same picture as plain order") {
    const Pixels bare = pixels(lump(flat));
    const Pixels laced = pixels(lump(woven));

    OUGHT(bare.held() && laced.held());
    OUGHT(bare.size() == laced.size());
    OUGHT(bare.stills()[0].rgba == laced.stills()[0].rgba);
}

// Transparency comes out as alpha zero, so that what is behind the picture shows through it.
TRIAL("GIF: the transparent index gives alpha zero") {
    const Pixels one = pixels(lump(clear));

    OUGHT(one.held());

    bool held = true;

    for (std::uint32_t y = 0; y < 5; ++y)
        for (std::uint32_t x = 0; x < 7; ++x) held = held && at(one, x, y).alpha == ((x + y) % 4 == 0 ? 0 : 255);

    OUGHT(held);
}

// --- Animation ------------------------------------------------------------------------------

TRIAL("GIF: an animation gives all its frames") {
    const Pixels one = pixels(lump(leaving));

    SAME(one.stills().size(), std::size_t{4});
    SAME(one.size().width, 16U);
    SAME(one.size().height, 12U);
}

// We follow the world, not the letter. The standard reads zero as "as fast as possible", which
// means a hundred frames per second and a strobe; for twenty years browsers have turned zero and
// one hundredth into ten hundredths, and half of the GIFs on the web are made for that. This file
// holds 4, 0, 1 and 12 hundredths of a second in turn, so the first and the last pass untouched,
// and the two in the middle are raised to a hundred milliseconds.
TRIAL("GIF: zero and shortest delays become a hundred milliseconds") {
    const Pixels one = pixels(lump(leaving));

    SAME(after(one, 0), 40LL);
    SAME(after(one, 1), 100LL);
    SAME(after(one, 2), 100LL);
    SAME(after(one, 3), 120LL);
}

// The "leave" disposal does not touch the canvas, so a pixel painted once and not painted again
// stays as it was; here the background is opaque black.
TRIAL("GIF: the leave disposal does not clear the canvas") {
    const Pixels one = pixels(lump(leaving));

    OUGHT(at(one, 0, 0, 0) == (Tone{255, 0, 0, 255}));
    OUGHT(at(one, 15, 11, 3) == (Tone{0, 0, 0, 255}));
}

// The "to background" disposal clears the rectangle of the previous frame, not the current one, and
// clears it to transparency, not to the background color from the header. That is what browsers do.
TRIAL("GIF: the to-background disposal leaves transparency") {
    const Pixels one = pixels(lump(clearing));

    OUGHT(at(one, 1, 1, 0) == (Tone{255, 0, 0, 255}));
    SAME(static_cast<int>(at(one, 1, 1, 3).alpha), 0);
    OUGHT(at(one, 10, 7, 3) == (Tone{255, 0, 0, 255}));
}

// The "restore previous" disposal returns to the state before the frame. The first frame has no
// previous state, so it returns to an empty canvas (checked against a browser, not guessed).
TRIAL("GIF: restore previous returns to an empty canvas") {
    const Pixels one = pixels(lump(backing));

    OUGHT(at(one, 15, 11, 0) == (Tone{0, 0, 0, 255}));
    SAME(static_cast<int>(at(one, 15, 11, 1).alpha), 0);
    OUGHT(at(one, 4, 3, 1) == (Tone{0, 255, 0, 255}));
}

// --- Budget and interruption ------------------------------------------------------------------

TRIAL("GIF: asking for some frames stops unpacking the animation") {
    SAME(pixels(lump(leaving), 1).stills().size(), std::size_t{1});
    SAME(pixels(lump(leaving), 3).stills().size(), std::size_t{3});
    SAME(pixels(lump(leaving), 9).stills().size(), std::size_t{4});
}

// The streaming form hands over a frame and forgets it. False from the caller means "enough" and is
// not an error, so a preview that wants only the first frame does not pay for the rest; without an
// action nothing is decoded.
TRIAL("GIF: the stream can be interrupted at any moment, and without an action does nothing") {
    std::size_t seen = 0;
    std::size_t length = 0;
    Size size;

    tal::media::decoded(
        lump(leaving),
        [&](std::span<const std::byte> canvas, Size given, std::chrono::milliseconds) {
            ++seen;
            size = given;
            length = canvas.size();

            return seen < 2;
        },
        tal::Optional<tal::media::Decoding>{});

    SAME(seen, std::size_t{2});
    OUGHT(size == (Size{16, 12}));
    SAME(length, std::size_t{16 * 12 * 4});

    tal::media::decoded(lump(leaving), tal::media::Taking{}, tal::Optional<tal::media::Decoding>{});
}

// --- Refusals ----------------------------------------------------------------------------------

// A canvas over the ceiling is refused before anything is allocated. GIF allows 65535 by 65535,
// which is seventeen gigabytes of RGBA, and that has to be a refusal, not an attempt.
TRIAL("GIF: a canvas over the ceiling is full, a canvas of nothing is malformed") {
    SAME(refusal(lump("474946383961ffffffff0000003b")), std::string{"full: The canvas of the GIF is over the ceiling."});
    SAME(refusal(lump("474946383961000004000000003b")), std::string{"malformed: The header of the GIF is broken."});
    SAME(refusal(lump("47494638396104")), std::string{"malformed: The header of the GIF is broken."});
}

TRIAL("GIF: a header alone gives no frame, and an unknown block ends reading instead of guessing on") {
    OUGHT(!pixels(lump("474946383961040004000000003b")).held());
    OUGHT(!pixels(lump("47494638396104000400000000ab")).held());
}

// A truncated file is not an empty file. A file manager also looks at files being copied and at
// damaged ones, and there the top half of a picture is a better answer than nothing. The same file
// cut short by six bytes gives four rows out of five, and the fifth stays transparent.
TRIAL("GIF: a truncated file gives as much of the picture as arrived") {
    const Bytes whole = lump(plain);
    const Pixels one = pixels(Bytes(whole.begin(), whole.end() - 6));

    OUGHT(one.held());
    SAME(one.size().height, 5U);
    OUGHT(at(one, 0, 3) == (Tone{0, 0, 255, 255}));
    SAME(static_cast<int>(at(one, 0, 4).alpha), 0);
}

TRIAL("GIF: a cut before the image data gives no frame") {
    const Bytes whole = lump(plain);

    OUGHT(!pixels(Bytes(whole.begin(), whole.begin() + 20)).held());
}

// --- A whole real file ---------------------------------------------------------------------------

TRIAL("GIF: a real file from someone else's encoder is read in full") {
    const Pixels one = pixels(lump(proof));

    OUGHT(one.held());
    SAME(one.size().width, 48U);
    SAME(one.size().height, 32U);
    SAME(one.stills().size(), std::size_t{16});
}

// The third and fifth frames have zero and one hundredth of a second written in the file; after the
// clamp both come out as a hundred milliseconds. The rest are 40, 70, 100, 130 and 160, which is
// what the encoder wrote.
TRIAL("GIF: the delays of a real file, with the clamp") {
    const Pixels one = pixels(lump(proof));
    const long long these[16] = {40, 70, 100, 100, 160, 100, 70, 100, 130, 160, 40, 70, 100, 130, 160, 40};
    bool held = true;

    for (std::size_t still = 0; still < 16; ++still) held = held && after(one, still) == these[still];

    OUGHT(held);
}

// A fingerprint of all frames, to the byte: if anything moves in the decoder, in composing the
// frames or in disposing of them, this number changes. It does not say what broke; it says that
// something did. The value was taken from a run checked pixel by pixel against an independent
// decoder; without that it would only be a stamp on our own result.
TRIAL("GIF: a fingerprint of all frames of a real file") {
    const Pixels one = pixels(lump(proof));
    std::uint64_t hash = 1469598103934665603ULL;

    for (const Still& said : one.stills())
        for (const std::byte what : said.rgba) {
            hash ^= std::to_integer<std::uint8_t>(what);
            hash *= 1099511628211ULL;
        }

    SAME(hash, 0xDD3730739D049EB1ULL);
}

// A few pixels written out, so that a failing fingerprint has somewhere to start from: the corner,
// which the first image does not reach, stays transparent; the square in the top left corner holds
// color number one of the palette.
TRIAL("GIF: selected pixels of a real file") {
    const Pixels one = pixels(lump(proof));

    SAME(static_cast<int>(at(one, 0, 0).alpha), 0);
    OUGHT(at(one, 4, 4) == (Tone{37, 71, 113, 255}));
}

// Shrunk while decoding: every frame comes at the size asked for, and the frames share the budget
// of the ceiling.
TRIAL("GIF: frames shrunk while decoding keep their count and delays") {
    tal::Loop loop;
    const Pixels one = tal::wait(loop, tal::media::decoded(lump(proof), Size{12, 8}, tal::Optional<tal::media::Decoding>{}));

    OUGHT(one.size() == (Size{12, 8}));
    SAME(one.stills().size(), std::size_t{16});
    SAME(one.stills()[15].rgba.size(), std::size_t{12 * 8 * 4});
    SAME(after(one, 4), 160LL);
}

// --- What no encoder writes ----------------------------------------------------------------------

// The header: only GIF87a and GIF89a, both sides of the canvas, the ceiling to the pixel (2829 by
// 2828 is over it, 4000 by 2000 just within), and a header cut short within a longer buffer, so
// that nothing is read past the bytes given.
TRIAL("GIF: the header: its signature, a side of nothing, the ceiling to the pixel, a cut") {
    const auto direct = [](std::span<const std::byte> given) {
        try {
            tal::media::gifting::gifted(given, [](auto, auto, auto) { return true; });
        } catch (const tal::Trouble& why) {
            return std::string{tal::spoken(why.kind())} + ": " + why.what();
        }

        return std::string{"none"};
    };
    const Bytes whole = lump("474946383961040004000000003b");

    SAME(direct(lump("4749463839620400040000003b")), std::string{"malformed: Not a GIF."});
    SAME(direct(lump("474946383961040000000000003b")), std::string{"malformed: The header of the GIF is broken."});
    SAME(direct(lump("4749463839610d0b0c0b0000003b")), std::string{"full: The canvas of the GIF is over the ceiling."});
    SAME(direct(lump("474946383961a00fd0070000003b")), std::string{"none"});
    SAME(direct(std::span<const std::byte>{whole}.first(9)), std::string{"malformed: The header of the GIF is broken."});
    SAME(direct(std::span<const std::byte>{whole}.first(12)), std::string{"malformed: The header of the GIF is broken."});
    SAME(direct(std::span<const std::byte>{whole}.first(13)), std::string{"none"});
}

// The codes: a clear in the middle starts the table anew, a stream may start without one, a code
// ahead of the table and anything after the end code are not pixels, and a code size below two is
// no stream at all.
TRIAL("GIF: the codes: a clear in the middle, none at the start, a code ahead, data after the end") {
    constexpr Tone red{255, 0, 0, 255};
    constexpr Tone green{0, 255, 0, 255};
    constexpr Tone blue{0, 0, 255, 255};
    const Pixels anew = pixels(lump(cleared));
    const Pixels plain_start = pixels(lump(bare));

    OUGHT(at(anew, 0, 0) == red && at(anew, 1, 0) == green);

    for (std::uint32_t x = 2; x < 6; ++x) OUGHT(at(anew, x, 0) == blue);

    OUGHT(at(plain_start, 0, 0) == red && at(plain_start, 1, 0) == green && at(plain_start, 2, 0) == red && at(plain_start, 3, 0) == green);
    OUGHT(at(pixels(lump(ahead)), 0, 0) == red);
    SAME(static_cast<int>(at(pixels(lump(ahead)), 1, 0).alpha), 0);
    OUGHT(at(pixels(lump(stopped)), 0, 0) == red);
    SAME(static_cast<int>(at(pixels(lump(stopped)), 1, 0).alpha), 0);
    OUGHT(!pixels(lump(narrow)).held());
}

TRIAL("GIF: twelve-bit codes on noise, to the fingerprint of Pillow's own decoding") {
    const Pixels one = pixels(lump(noise));
    std::uint64_t hash = 1469598103934665603ULL;

    for (const std::byte what : one.stills().front().rgba) {
        hash ^= std::to_integer<std::uint8_t>(what);
        hash *= 1099511628211ULL;
    }

    OUGHT(one.size() == (Size{64, 64}));
    SAME(hash, 0x1B5A2E2E4122000CULL);
    OUGHT(at(one, 5, 7) == (Tone{120, 28, 242, 255}));
}

// An index beyond the palette paints nothing, and a frame reaching past the canvas is cut to it,
// also when it is cleared to the background.
TRIAL("GIF: an index beyond the palette is clear; a frame past the canvas is cut to it") {
    const Pixels two = pixels(lump(beyond));
    const Pixels cut = pixels(lump(outside));

    OUGHT(at(two, 0, 0) == (Tone{0, 0, 0, 255}));
    SAME(static_cast<int>(at(two, 1, 0).alpha), 0);
    SAME(cut.stills().size(), std::size_t{2});
    OUGHT(at(cut, 2, 2, 0) == (Tone{255, 0, 0, 255}) && at(cut, 3, 3, 0) == (Tone{255, 0, 0, 255}));
    SAME(static_cast<int>(at(cut, 1, 1, 0).alpha), 0);
    OUGHT(at(cut, 0, 0, 1) == (Tone{0, 255, 0, 255}));
    SAME(static_cast<int>(at(cut, 2, 2, 1).alpha), 0);
    SAME(static_cast<int>(at(cut, 3, 3, 1).alpha), 0);
}

// The blocks between images: a reserved disposal leaves the canvas as it is, a comment is no
// control, an unknown byte and a frame whose data is empty end the reading, and a control holds for
// the one frame after it.
TRIAL("GIF: a reserved disposal, a comment, an unknown byte, an empty frame, control for one frame") {
    const Pixels kept = pixels(lump(reserved));
    const Pixels once = pixels(lump(reset));

    OUGHT(at(kept, 0, 0, 1) == (Tone{0, 255, 0, 255}) && at(kept, 1, 0, 1) == (Tone{255, 0, 0, 255}));
    SAME(after(pixels(lump(commented)), 0), 100LL);
    OUGHT(!pixels(lump(lost)).held());
    OUGHT(!pixels(lump(silent)).held());
    SAME(once.stills().size(), std::size_t{2});
    SAME(after(once, 0), 500LL);
    SAME(after(once, 1), 100LL);
}
