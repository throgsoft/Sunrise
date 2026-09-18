#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../progression/season_pass_reward_catalog.h"

namespace sunrise::state::runtime::detail::bounty_policy {

/**
 * Exact repeatable bounty hashes from the target manifest's non-localized stack namespaces.
 * This is evidence for cadence identity only. The 4,000-XP amount remains authored Sunrise policy.
 */
inline constexpr std::array<std::uint32_t, 159> kRepeatableBounties{
    3801019451U, 3801019450U, 3801019449U, 3801019448U, 3801019455U, 3801019454U, 3801019453U,
    3801019452U, 3801019443U, 3801019442U, 4228939450U, 4228939451U, 4228939448U, 4228939449U,
    4228939454U, 4228939455U, 4228939452U, 4228939453U, 4228939442U, 1932203931U, 1486111804U,
    782388144U,  3609320918U, 3089242929U, 1494697569U, 789196172U,  3512551717U, 2121605528U,
    711780003U,  3529619172U, 1387738947U, 3292943447U, 1491123344U, 3503381224U, 2212074115U,
    833201720U,  2514375981U, 2420899653U, 729780848U,  3112855567U, 90105468U,   429152515U,
    4104555523U, 1115261524U, 1723091344U, 1400452240U, 3658765719U, 3351200278U, 2731635990U,
    3675498641U, 3636655077U, 1777618773U, 484780086U,  1774447862U, 4059065847U, 903424492U,
    2940269474U, 4235693605U, 211068288U,  2462005227U, 3109359405U, 3109359404U, 3109359407U,
    3109359406U, 3109359401U, 3109359400U, 3109359403U, 3109359402U, 3109359397U, 3109359396U,
    204755896U,  204755897U,  204755898U,  204755899U,  204755900U,  204755901U,  204755902U,
    2592950299U, 2592950298U, 2592950297U, 2592950296U, 2592950303U, 2592950302U, 2592950301U,
    2592950300U, 2592950291U, 2592950290U, 1718124766U, 1718124767U, 1718124764U, 1718124765U,
    1718124762U, 1718124763U, 2805818748U, 3990791875U, 3683313874U, 3514703992U, 3512622966U,
    692493341U,  13409814U,   3312152065U, 1913307766U, 3357287242U, 391818767U,  3134246761U,
    2703627837U, 2816193016U, 874160405U,  146278979U,  4114666828U, 1315013095U, 3155878395U,
    2653579669U, 2428565738U, 3141795163U, 3696295486U, 1453527970U, 2133176491U, 3697234118U,
    3178157690U, 2068952036U, 3191735197U, 2675137426U, 1244249306U, 3038846813U, 2695042788U,
    857633000U,  3971836963U, 3731062163U, 1812672687U, 501758776U,  2903204355U, 3560267478U,
    3209637560U, 1428740103U, 1258056755U, 2497038231U, 1113031720U, 2003717903U, 1983539982U,
    1058022683U, 1899370350U, 1561597496U, 2285406418U, 31218063U,   310094086U,  3219965079U,
    2213620835U, 711490543U,  2267981985U, 130487539U,  2328867053U, 4084493733U, 2858316395U,
    3438972653U, 847805104U,  3721656876U, 1764928973U, 341098154U,
};

/** Exact Petra daily identities; their 97,200-second lifetime overlaps other cadence families. */
inline constexpr std::array<std::uint32_t, 6> kDreamingCityDailyBounties{
    1391363409U,
    2662637005U,
    4095766153U,
    2271002093U,
    2494871935U,
    1912981534U,
};

/** Exact Petra weekly identities, including the six rotations and Audience with the Queen. */
inline constexpr std::array<std::uint32_t, 7> kDreamingCityWeeklyBounties{
    3337739523U,
    128980839U,
    1147672297U,
    2338128705U,
    3207732940U,
    542328999U,
    871673916U,
};

/** Exact Trials daily identities from target-build rows 14023-14040. */
inline constexpr std::array<std::uint32_t, 18> kTrialsDailyBounties{
    1807819922U,
    2719669554U,
    2469435739U,
    1649087790U,
    3647168216U,
    3636568846U,
    2849007504U,
    3238546247U,
    836380479U,
    712137159U,
    303384168U,
    2398816281U,
    4126111451U,
    550008052U,
    1761112788U,
    2943701307U,
    1244906310U,
    1083494845U,
};

/** Exact Trials weekly identities other than End Game, whose rotation remains unresolved below. */
inline constexpr std::array<std::uint32_t, 8> kTrialsWeeklyBounties{
    3051422226U,
    3497386880U,
    42252934U,
    58287472U,
    225464052U,
    1148638263U,
    2780182450U,
    3099386391U,
};

/** Exact standard Werner weekly identities. The adjacent intro pair is intentionally excluded. */
inline constexpr std::array<std::uint32_t, 12> kWernerWeeklyBounties{
    3042981914U,
    3042981915U,
    3042981912U,
    3970388282U,
    3970388283U,
    3970388280U,
    1262245655U,
    1262245654U,
    1262245653U,
    504411738U,
    504411739U,
    504411736U,
};

/** Exact Hawthorne clan weeklies whose activity-neutral gear reward uses the world seed below. */
inline constexpr std::array<std::uint32_t, 8> kHawthorneWorldBounties{
    2655712924U,
    459676116U,
    852248609U,
    1794825180U,
    2850564884U,
    2729271716U,
    1441461394U,
    1381806136U,
};

/** A Calus Affair uses Hawthorne's generic gear source and a legacy, un-tiered power marker. */
inline constexpr std::array<std::uint32_t, 1> kHawthorneLegacyPowerfulBounties{504379403U};

/** Weekly Ada bounties canonically award one class-compatible Black Armory armor piece. */
inline constexpr std::array<std::uint32_t, 2> kBlackArmoryArmorBounties{
    1128713411U,
    3179992453U,
};

/** Enhancement Cores are the exact multi-stack exception observed in the target inventory. */
inline constexpr std::uint32_t kEnhancementCoreHash = 3853748946U;

/** Exact profile-wallet row whose bounty overflow saturates instead of blocking redemption. */
inline constexpr std::uint32_t kGlimmerHash = 3159615086U;

/**
 * Standard and focused Arrivals Umbral Engrams from the target manifest.
 *
 * These are real instance items in the ten-row Engrams bucket. Row 12391 is intentionally absent:
 * it is a presentation definition in the hidden display bucket, not an inventory engram.
 */
inline constexpr std::array<std::uint32_t, 22> kArrivalsUmbralEngrams{
    3453985408U, 2842241466U, 2842241467U, 2842241464U, 2509979463U, 2509979462U,
    2509979461U, 2509979460U, 4099020041U, 4099020040U, 4099020043U, 3901167404U,
    112700765U,  40166197U,   3544458615U, 129985070U,  1835325612U, 520769841U,
    847709274U,  847709275U,  847709272U,  847709273U,
};

/** Rare Ada bounties whose Legendary Gear selector historically drew forge gear or class armor. */
inline constexpr std::array<std::uint32_t, 8> kBlackArmoryRareBounties{
    89995072U,
    346974120U,
    774885924U,
    1085775425U,
    1126196126U,
    1727646073U,
    3357615447U,
    4237578370U,
};

/** Exact weekly Revelry bounty identities; their wrapper fixes the armor slot below. */
inline constexpr std::array<std::uint32_t, 6> kRevelryWeeklyBounties{
    78926733U,
    3180273074U,
    3180273075U,
    3180273072U,
    3180273073U,
    3180273078U,
};

/** Exact S11 Iron Banner weekly bounty identities. */
inline constexpr std::array<std::uint32_t, 7> kIronBannerWeeklyBounties{
    3063752398U,
    2099765145U,
    1960358731U,
    4068460499U,
    1284302942U,
    3780376499U,
    2352873759U,
};

/** Luna's Recall declares the legacy un-tiered Powerful Gear wrapper. */
inline constexpr std::array<std::uint32_t, 1> kLunasRecallWeeklyBounties{1264945960U};

/** Purification Ritual variants share the same Dreaming City activity reward family. */
inline constexpr std::array<std::uint32_t, 5> kPurificationRitualBounties{
    346263927U,
    1123124244U,
    2256480378U,
    455233900U,
    2663541547U,
};

/** Invitations of the Nine; payout is known but their quest/activity claim lane is separate. */
inline constexpr std::array<std::uint32_t, 9> kInvitationsOfTheNineBounties{
    2716564042U,
    2716564041U,
    2716564040U,
    2716564047U,
    2716564046U,
    2716564045U,
    2716564044U,
    2716564035U,
    2716564034U,
};

/** Petra challenge and Ascendant bounties with a Dreaming City gear reward. */
inline constexpr std::array<std::uint32_t, 13> kDreamingCityGearBounties{
    3337739523U,
    128980839U,
    1147672297U,
    2338128705U,
    3207732940U,
    542328999U,
    871673916U,
    1391363409U,
    2662637005U,
    4095766153U,
    2271002093U,
    2494871935U,
    1912981534U,
};

/** Drifter's ordinary S11 weekly bounties; Prime/Reckoning uses the stateful rules below. */
inline constexpr std::array<std::uint32_t, 3> kGambitLegacyBounties{
    2839632402U,
    2839632400U,
    2839632405U,
};

/** `Yes Sir, I'm a Closer`, whose Legendary Gear selector historically paid one Prime weapon. */
inline constexpr std::array<std::uint32_t, 1> kGambitPrimeWeaponBounties{4132751916U};

/**
 * Prime role weeklies whose displayed Head/Synthesizer rows are account-state selectors, not
 * concrete inventory definitions. Paired hashes distinguish an upgrade-bearing vendor row from
 * the role-head-only row used after the Synthesizer has reached its ceiling.
 */
inline constexpr std::array<std::uint32_t, 8> kGambitPrimeRoleWeeklyBounties{
    53973468U,
    2724713067U,
    53973469U,
    2496824396U,
    53973470U,
    1417485797U,
    53973471U,
    535212982U,
};

/** Hawthorne raid-challenge families, split because each raid owns a different pool. */
inline constexpr std::array<std::uint32_t, 5> kLastWishBounties{
    2836954349U,
    1250327262U,
    3871581136U,
    1568895666U,
    4007940282U,
};
inline constexpr std::array<std::uint32_t, 3> kScourgeBounties{
    1348944144U,
    3415614992U,
    1381881897U,
};
inline constexpr std::array<std::uint32_t, 3> kCrownBounties{
    2459033425U,
    2459033426U,
    2459033427U,
};

/**
 * Trials' weekly three-win reward. The target manifest does not preserve its weekly rotation, so
 * Sunrise uses the complete class-compatible challenge set below as an explicitly authored
 * fallback rather than claiming one member is the canonical reward for this week.
 */
inline constexpr std::array<std::uint32_t, 1> kTrialsEndGameFallbackBounties{45262603U};

/** Native semantic markers and their uniquely named account progression destinations. */
inline constexpr std::uint32_t kValorRankPointsMarkerHash = 1808687944U;
inline constexpr std::uint32_t kValorRankProgressionHash = 3882308435U;
inline constexpr std::int32_t kSunriseValorRankSaturationCap = 2'000;
inline constexpr std::uint32_t kInfamyRankPointsMarkerHash = 372496383U;
inline constexpr std::uint32_t kInfamyRankProgressionHash = 2772425241U;
inline constexpr std::int32_t kSunriseInfamyRankSaturationCap = 15'000;

/** Clan XP has no supported clan-scoped State object; it is explicitly suppressed. */
inline constexpr std::uint32_t kClanExperienceMarkerHash = 304443327U;

/** Gambit Prime's role-head and Synthesizer rows are semantic account-unlock selectors. */
inline constexpr std::array<std::uint32_t, 4> kGambitPrimeRoleHeadMarkerHashes{
    3007303932U,
    1355700046U,
    4041437604U,
    1045201464U,
};
inline constexpr std::uint32_t kGambitPrimeSynthesizerUpgradeMarkerHash = 596773932U;

/**
 * Current Armor 2.0 Prime helmet definitions in the target build.
 *
 * Rows are role-marker order (Reaper, Invader, Collector, Sentry), then character class order
 * (Titan, Hunter, Warlock), then earned tier (Illicit, Outlawed, Notorious). The older eight-socket
 * collectible definitions remain in the manifest but are not the target build's current drops.
 */
struct GambitPrimeRoleHelmetRow {
    std::uint32_t markerHash{};
    std::array<std::array<std::uint32_t, 3>, 3> classTiers{};
};

inline constexpr std::array<GambitPrimeRoleHelmetRow, 4> kGambitPrimeRoleHelmets{{
    {3007303932U,
     {{{223681334U, 223681335U, 223681332U},
       {1295793306U, 1295793307U, 1295793304U},
       {1208982393U, 1208982392U, 1208982395U}}}},
    {1355700046U,
     {{{3636943394U, 3636943395U, 3636943392U},
       {2593076934U, 2593076935U, 2593076932U},
       {1951201411U, 1951201410U, 1951201409U}}}},
    {4041437604U,
     {{{975478396U, 975478397U, 975478398U},
       {2698109344U, 2698109345U, 2698109346U},
       {2565812705U, 2565812704U, 2565812707U}}}},
    {1045201464U,
     {{{2187982746U, 2187982747U, 2187982744U},
       {3220030414U, 3220030415U, 3220030412U},
       {3660501109U, 3660501108U, 3660501111U}}}},
}};

/** Historical Gambit Prime/Reckoning weapon membership used by the 14324 selector. */
inline constexpr std::array<std::uint32_t, 10> kGambitPrimeWeapons{
    2744715540U,
    736901634U,
    821154603U,
    715338174U,
    755130877U,
    2199171672U,
    3504336176U,
    299665907U,
    1115104187U,
    3116356268U,
};

/** End Game's engram is a rotation wrapper, never a concrete item to mint. */
inline constexpr std::uint32_t kTrialsEngramMarkerHash = 1526650446U;

/** Revelry display wrappers select a fixed armor slot and must never be minted themselves. */
inline constexpr std::uint32_t kRevelryArmsMarkerHash = 2501601653U;
inline constexpr std::uint32_t kRevelryChestMarkerHash = 514936467U;
inline constexpr std::uint32_t kRevelryHeadMarkerHash = 3818379434U;
inline constexpr std::uint32_t kRevelryLegsMarkerHash = 323881355U;
inline constexpr std::uint32_t kRevelryClassItemMarkerHash = 1508024268U;

struct RevelryArmorRewardRow {
    std::uint32_t markerHash{};
    /** Concrete item hashes in CharacterClass order: Titan, Hunter, Warlock. */
    std::array<std::uint32_t, 3> classItems{};
};

inline constexpr std::array<RevelryArmorRewardRow, 5> kRevelryArmorRewards{{
    {kRevelryArmsMarkerHash, {1556831535U, 419435523U, 1273510836U}},
    {kRevelryChestMarkerHash, {1056992393U, 3965417933U, 1376763596U}},
    {kRevelryHeadMarkerHash, {2328435454U, 2477028154U, 492834021U}},
    {kRevelryLegsMarkerHash, {1706874193U, 2764769717U, 1561249470U}},
    {kRevelryClassItemMarkerHash, {2837295684U, 2120905920U, 4075522049U}},
}};

/** One exact Werner weekly or intro bounty replacement recovered from the parallel manifest rows.
 */
struct WernerTreasureMapTransition {
    std::uint32_t bountyHash{};
    std::uint32_t mapHash{};
};

/**
 * Werner's standard bounty rows 14362-14373 align one-for-one with map rows 14350-14361.
 * The intro pair is the adjacent 14379->14380 pair. Redemption replaces the source with its map;
 * it does not pay the map's chest rewards.
 */
inline constexpr std::array<WernerTreasureMapTransition, 13> kWernerTreasureMapTransitions{{
    {3042981914U, 561724901U},
    {3042981915U, 561724902U},
    {3042981912U, 561724896U},
    {3970388282U, 561724898U},
    {3970388283U, 561724899U},
    {3970388280U, 561724908U},
    {1262245655U, 2583850831U},
    {1262245654U, 2583850830U},
    {1262245653U, 2583850829U},
    {504411738U, 2583850828U},
    {504411739U, 2583850827U},
    {504411736U, 2583850825U},
    {2997169129U, 1492273683U},
}};

/**
 * Exact map/chest stage. Holding one drives the client's Director X; it is chest-claim
 * authorization, not an inventory-redemption or dismantle target. Its conditional activity chest,
 * gear, Chalice-aware rune selection, and Imperial payout remain unresolved server work.
 */
inline constexpr std::array<std::uint32_t, 13> kWernerTreasureMaps{
    561724901U,
    561724902U,
    561724896U,
    561724898U,
    561724899U,
    561724908U,
    2583850831U,
    2583850830U,
    2583850829U,
    2583850828U,
    2583850827U,
    2583850825U,
    1492273683U,
};

/** All known Werner stages, retained for explicit source-family classification. */
inline constexpr std::array<std::uint32_t, 26> kWernerBountiesAndMaps{
    3042981914U, 3042981915U, 3042981912U, 3970388282U, 3970388283U, 3970388280U, 1262245655U,
    1262245654U, 1262245653U, 504411738U,  504411739U,  504411736U,  561724901U,  561724902U,
    561724896U,  561724898U,  561724899U,  561724908U,  2583850831U, 2583850830U, 2583850829U,
    2583850828U, 2583850827U, 2583850825U, 2997169129U, 1492273683U,
};

/** The real profile currency; the Puny Purse hash remains a semantic display wrapper. */
inline constexpr std::uint32_t kImperialsHash = 1642918584U;

/**
 * Explicit Sunrise fallback. Historical chest observations say 100, but the manifest preserves
 * only one Puny Purse wrapper and contains no native wrapper-to-currency quantity relation.
 */
inline constexpr std::int32_t kSunriseWernerImperialsFallback = 100;

/** Reuse upstream's installed-season world reward policy instead of duplicating its membership. */
inline constexpr auto& kWorldWeapons = progression::season_pass::kLegendaryEngramWeapons;
inline constexpr auto& kWorldHunterArmor = progression::season_pass::kLegendaryHunterArmour;
inline constexpr auto& kWorldTitanArmor = progression::season_pass::kLegendaryTitanArmour;
inline constexpr auto& kWorldWarlockArmor = progression::season_pass::kLegendaryWarlockArmour;
/** Black Armory weekly reward membership, five armor pieces per class. */
inline constexpr std::array<std::uint32_t, 5> kBlackArmoryHunterArmor{
    240988159U,
    2791841721U,
    4106007668U,
    3086191374U,
    3457205569U,
};
inline constexpr std::array<std::uint32_t, 5> kBlackArmoryTitanArmor{
    89175933U,
    866063619U,
    4059853946U,
    524862116U,
    2413328031U,
};
inline constexpr std::array<std::uint32_t, 5> kBlackArmoryWarlockArmor{
    3128915572U,
    2153602188U,
    91359169U,
    2389815461U,
    3416654206U,
};

/** Seven forge weapons added to the armor membership for rare-bounty uniform fallback draws. */
inline constexpr std::array<std::uint32_t, 7> kBlackArmoryForgeWeapons{
    603242241U,
    93253474U,
    3843477312U,
    2575506895U,
    421573768U,
    1449922174U,
    3704653637U,
};

/** Revelry class sets. Marker-aware selection below picks exactly one slot from these rows. */
inline constexpr std::array<std::uint32_t, 5> kRevelryTitanArmor{
    1556831535U,
    1056992393U,
    2328435454U,
    1706874193U,
    2837295684U,
};
inline constexpr std::array<std::uint32_t, 5> kRevelryHunterArmor{
    419435523U,
    3965417933U,
    2477028154U,
    2764769717U,
    2120905920U,
};
inline constexpr std::array<std::uint32_t, 5> kRevelryWarlockArmor{
    1273510836U,
    1376763596U,
    492834021U,
    1561249470U,
    4075522049U,
};

/** Dreaming City activity membership, with cosmetics excluded. */
inline constexpr std::array<std::uint32_t, 7> kDreamingCityWeapons{
    640114618U,
    334171687U,
    3242168339U,
    3297863558U,
    346136302U,
    3740842661U,
    1644160541U,
};
inline constexpr std::array<std::uint32_t, 5> kDreamingCityHunterArmor{
    1705856569U,
    1593474975U,
    3306564654U,
    2824453288U,
    344548395U,
};
inline constexpr std::array<std::uint32_t, 5> kDreamingCityTitanArmor{
    2503434573U,
    4070309619U,
    1980768298U,
    4097166900U,
    3174233615U,
};
inline constexpr std::array<std::uint32_t, 5> kDreamingCityWarlockArmor{
    2761343386U,
    2859583726U,
    3602032567U,
    185695659U,
    188778964U,
};

/** Authored S11 Gambit fallback: eight universal weapons plus current class armor. */
inline constexpr std::array<std::uint32_t, 8> kGambitWeapons{
    1789347249U,
    4077196130U,
    2712244741U,
    2034817450U,
    2653316158U,
    2217366863U,
    3100452337U,
    991314988U,
};
inline constexpr std::array<std::uint32_t, 5> kGambitHunterArmor{
    606902507U,
    1314666277U,
    436615288U,
    1267361154U,
    1698660093U,
};
inline constexpr std::array<std::uint32_t, 5> kGambitTitanArmor{
    3863492689U,
    1811579911U,
    2993008662U,
    2002452096U,
    3694642467U,
};
inline constexpr std::array<std::uint32_t, 5> kGambitWarlockArmor{
    4122447870U,
    725297842U,
    896081219U,
    754069623U,
    671423576U,
};

/** S11 Iron Banner activity membership; uniform weights are authored Sunrise policy. */
inline constexpr std::array<std::uint32_t, 8> kIronBannerWeapons{
    1690783811U,
    65611680U,
    136525518U,
    1972985595U,
    1982711279U,
    3169616514U,
    2108920981U,
    4292849692U,
};
inline constexpr std::array<std::uint32_t, 5> kIronBannerTitanArmor{
    391384020U,
    3517179757U,
    1571959827U,
    3763521327U,
    4201843274U,
};
inline constexpr std::array<std::uint32_t, 5> kIronBannerHunterArmor{
    3624199242U,
    3671337107U,
    4039932861U,
    3406291173U,
    1248530160U,
};
inline constexpr std::array<std::uint32_t, 5> kIronBannerWarlockArmor{
    2717305289U,
    125833536U,
    2522706952U,
    3115740538U,
    4224804453U,
};

/**
 * Target-build Trials challenge membership (collectible source hash 3390015730).
 *
 * The source proves membership, rarity, and class compatibility, but not the missing weekly
 * rotation or its weights. End Game therefore uses one deterministic uniform draw from the six
 * weapons plus the selected class's five armor pieces. The two Trials Exotic cosmetics have
 * different source hashes and are deliberately excluded.
 */
inline constexpr std::array<std::uint32_t, 6> kTrialsWeapons{
    1907698332U,
    679281855U,
    958384347U,
    2478792241U,
    1697682876U,
    3164743584U,
};
inline constexpr std::array<std::uint32_t, 5> kTrialsHunterArmor{
    3275117874U,
    3741471003U,
    938618741U,
    2616071821U,
    18990920U,
};
inline constexpr std::array<std::uint32_t, 5> kTrialsTitanArmor{
    435339366U,
    3702689847U,
    1127943297U,
    791799769U,
    2376585692U,
};
inline constexpr std::array<std::uint32_t, 5> kTrialsWarlockArmor{
    1637326795U,
    3829990714U,
    2883045518U,
    1215952756U,
    3201140055U,
};

/** Raid-wide fallback membership; raid exotics and encounter weighting are excluded. */
inline constexpr std::array<std::uint32_t, 8> kLastWishWeapons{
    2721249463U,
    601592879U,
    4094657108U,
    654370424U,
    568515759U,
    3799980700U,
    686951703U,
    2545083870U,
};
inline constexpr std::array<std::uint32_t, 5> kLastWishHunterArmor{
    1127835600U,
    2868042232U,
    1646520469U,
    1190016345U,
    1444894250U,
};
inline constexpr std::array<std::uint32_t, 5> kLastWishTitanArmor{
    576683388U,
    3143067364U,
    16387641U,
    4219088013U,
    3874578566U,
};
inline constexpr std::array<std::uint32_t, 5> kLastWishWarlockArmor{
    3492720019U,
    776723133U,
    2280287728U,
    3445582154U,
    3227674085U,
};

inline constexpr std::array<std::uint32_t, 4> kScourgeWeapons{
    2753269585U,
    2186258845U,
    1931556011U,
    1664372054U,
};
inline constexpr std::array<std::uint32_t, 5> kScourgeHunterArmor{
    2334017923U,
    300528205U,
    2750983488U,
    96643258U,
    384384821U,
};
inline constexpr std::array<std::uint32_t, 5> kScourgeTitanArmor{
    1989682895U,
    3491990569U,
    977326564U,
    2719710110U,
    2564183153U,
};
inline constexpr std::array<std::uint32_t, 5> kScourgeWarlockArmor{
    2286640864U,
    4092373800U,
    1499503877U,
    583145321U,
    940003738U,
};

inline constexpr std::array<std::uint32_t, 4> kCrownWeapons{
    1286686760U,
    1496419775U,
    2338088853U,
    3861448240U,
};
inline constexpr std::array<std::uint32_t, 5> kCrownHunterArmor{
    4017853847U,
    942205921U,
    2149271612U,
    326149062U,
    1107067065U,
};
inline constexpr std::array<std::uint32_t, 5> kCrownTitanArmor{
    1595987387U,
    3406713877U,
    2104205416U,
    1129634130U,
    4450861U,
};
inline constexpr std::array<std::uint32_t, 5> kCrownWarlockArmor{
    3211894260U,
    3381758732U,
    1319515713U,
    2472794149U,
    3499632894U,
};

} // namespace sunrise::state::runtime::detail::bounty_policy
