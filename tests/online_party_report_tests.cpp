// Party snapshots: the turtle table, which character select states make a report, the integer that crosses from TMNT_r to
// the exe, and when a report is sent. Headless.
// Run: tmnt_online_party_report_tests

#include <cstdio>
#include <string>

#include "online/party_report.h"

using namespace tmnt::online;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

constexpr int kLeonardo = 0;
constexpr int kMichelangelo = 1;
constexpr int kRaphael = 2;
constexpr int kDonatello = 3;
constexpr int32_t kHostStation = 7;
constexpr int32_t kGuestStation = 9;
const std::string kMatch = "0011223344556677";

PartySeat Seat(int choice, int32_t station, bool confirmed = true) { return {choice, confirmed ? 1 : 0, station}; }

PartySnapshot Snapshot(std::initializer_list<int> turtles, int host) {
  PartySnapshot snapshot;
  for (int turtle : turtles) snapshot.characters[snapshot.size++] = uint8_t(turtle);
  snapshot.host = int8_t(host);
  return snapshot;
}

void TestAllFourTurtlesAreKnownAndNothingElse() {
  CHECK(kTurtleCount == 4);
  for (int turtle : {kLeonardo, kMichelangelo, kRaphael, kDonatello}) CHECK(ValidTurtle(turtle));
  CHECK(!ValidTurtle(-1));
  CHECK(!ValidTurtle(4));
  CHECK(!ValidTurtle(1000));
}

void TestPartiesOfTwoThreeAndFour() {
  auto two = BuildPartySnapshot({Seat(kRaphael, kHostStation), Seat(kLeonardo, kGuestStation)}, kHostStation);
  CHECK(two && two->size == 2 && two->characters[0] == kLeonardo && two->characters[1] == kRaphael);
  auto three = BuildPartySnapshot({Seat(kDonatello, kHostStation), Seat(kLeonardo, kGuestStation), Seat(kRaphael, 11)}, kHostStation);
  CHECK(three && three->size == 3 && three->characters[2] == kDonatello);
  auto four = BuildPartySnapshot({Seat(kDonatello, kHostStation), Seat(kLeonardo, kGuestStation), Seat(kRaphael, 11),
                                  Seat(kMichelangelo, 13)}, kHostStation);
  CHECK(four && four->size == 4 && four->characters == (std::array<uint8_t, 4>{0, 1, 2, 3}));
}

void TestDuplicateTurtlesStayInTheSnapshot() {
  auto twins = BuildPartySnapshot({Seat(kLeonardo, kHostStation), Seat(kLeonardo, kGuestStation), Seat(kRaphael, 11)}, kHostStation);
  CHECK(twins && twins->size == 3 && twins->characters[0] == kLeonardo && twins->characters[1] == kLeonardo);
}

void TestTheHostTurtleComesFromTheStationNotTheSeatOrder() {
  auto late = BuildPartySnapshot({Seat(kLeonardo, kGuestStation), Seat(kRaphael, 11), Seat(kDonatello, kHostStation)}, kHostStation);
  CHECK(late && late->host == kDonatello);
  CHECK(!BuildPartySnapshot({Seat(kLeonardo, kGuestStation), Seat(kRaphael, 11)}, kHostStation));
}

void TestIncompleteSelectionIsNotReported() {
  CHECK(!BuildPartySnapshot({Seat(kLeonardo, kHostStation), Seat(kRaphael, kGuestStation, false)}, kHostStation));
  CHECK(!BuildPartySnapshot({Seat(-1, kHostStation), Seat(kRaphael, kGuestStation)}, kHostStation));
  CHECK(!BuildPartySnapshot({Seat(4, kHostStation), Seat(kRaphael, kGuestStation)}, kHostStation));
}

void TestUnbookedSeatsAreNotPlayers() {
  auto two = BuildPartySnapshot({Seat(kLeonardo, kHostStation), Seat(0, kNoStation, false), Seat(kRaphael, kGuestStation),
                                 Seat(3, kNoStation, true)}, kHostStation);
  CHECK(two && two->size == 2);
}

void TestASinglePlayerIsNotReported() {
  CHECK(!BuildPartySnapshot({Seat(kLeonardo, kHostStation)}, kHostStation));
  CHECK(!BuildPartySnapshot({}, kHostStation));
  CHECK(BookedSeats({Seat(kLeonardo, kHostStation)}) == 1);
  CHECK(BookedSeats({}) == 0);
}

void TestTheSnapshotSurvivesTheCvarInteger() {
  for (const PartySnapshot& snapshot : {Snapshot({kLeonardo, kDonatello}, kDonatello),
                                        Snapshot({kLeonardo, kLeonardo, kRaphael}, kLeonardo),
                                        Snapshot({kLeonardo, kMichelangelo, kRaphael, kDonatello}, kRaphael)}) {
    for (uint32_t sequence : {1u, 2u, 0x7fffu, 0xffffu, 0x12345u}) {
      const int32_t encoded = EncodePartySnapshot(snapshot, sequence);
      CHECK(encoded > 0);
      auto decoded = DecodePartySnapshot(encoded);
      CHECK(decoded && *decoded == snapshot);
    }
  }
  CHECK(EncodePartySnapshot(Snapshot({0, 1}, 0), 1) != EncodePartySnapshot(Snapshot({0, 1}, 0), 2));
  CHECK(!DecodePartySnapshot(0));
  CHECK(!DecodePartySnapshot(-5));
  CHECK(!DecodePartySnapshot(1));         // one seat, no host
  CHECK(!DecodePartySnapshot(0x1fff));    // turtle fields out of range
  CHECK(!DecodePartySnapshot(1 | (2 << 3)));  // no host
}

void TestOnlyTheHostOfARealOnlinePartyMayReport() {
  CHECK(CanReportParty(true, true, 1, 2));
  CHECK(CanReportParty(true, true, 1, 4));
  CHECK(!CanReportParty(true, true, 2, 2));   // guest
  CHECK(!CanReportParty(true, true, 1, 1));   // alone
  CHECK(!CanReportParty(true, false, 0, 0));  // no party: offline, local play
  CHECK(!CanReportParty(false, true, 1, 2));  // not signed in, Online off
}

void TestTheSameSnapshotIsSentOncePerParty() {
  PartyReporter reporter;
  const PartySnapshot party = Snapshot({kLeonardo, kRaphael}, kRaphael);
  CHECK(!reporter.Take(kMatch, 0));
  reporter.Offer(party);
  CHECK(reporter.Take(kMatch, 0));
  CHECK(!reporter.pending());
  reporter.Offer(party);
  CHECK(!reporter.Take(kMatch, 500));
  CHECK(!reporter.pending());  // dropped, not held back
}

void TestAChangedSnapshotWaitsForTheGap() {
  PartyReporter reporter;
  reporter.Offer(Snapshot({kLeonardo, kRaphael}, kRaphael));
  CHECK(reporter.Take(kMatch, 100));
  reporter.Offer(Snapshot({kLeonardo, kDonatello}, kDonatello));
  CHECK(!reporter.Take(kMatch, 100 + kPartyReportGapSeconds - 1));
  CHECK(reporter.pending());
  CHECK(reporter.Take(kMatch, 100 + kPartyReportGapSeconds));
}

void TestTheSameTurtlesInANewPartyAreANewSample() {
  PartyReporter reporter;
  const PartySnapshot party = Snapshot({kLeonardo, kRaphael}, kRaphael);
  reporter.Offer(party);
  CHECK(reporter.Take(kMatch, 0));
  reporter.Offer(party);
  CHECK(!reporter.Take("7766554433221100", 1));  // the gap still applies
  CHECK(reporter.Take("7766554433221100", kPartyReportGapSeconds));
}

void TestClearForgetsAnUnsentSnapshot() {
  PartyReporter reporter;
  reporter.Offer(Snapshot({kLeonardo, kRaphael}, kRaphael));
  reporter.Clear();
  CHECK(!reporter.pending() && !reporter.Take(kMatch, 0));
}

}  // namespace

int main() {
  TestAllFourTurtlesAreKnownAndNothingElse();
  TestPartiesOfTwoThreeAndFour();
  TestDuplicateTurtlesStayInTheSnapshot();
  TestTheHostTurtleComesFromTheStationNotTheSeatOrder();
  TestIncompleteSelectionIsNotReported();
  TestUnbookedSeatsAreNotPlayers();
  TestASinglePlayerIsNotReported();
  TestTheSnapshotSurvivesTheCvarInteger();
  TestOnlyTheHostOfARealOnlinePartyMayReport();
  TestTheSameSnapshotIsSentOncePerParty();
  TestAChangedSnapshotWaitsForTheGap();
  TestTheSameTurtlesInANewPartyAreANewSample();
  TestClearForgetsAnUnsentSnapshot();
  std::printf("online party report tests: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
