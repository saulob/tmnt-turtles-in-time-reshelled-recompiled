// Guest heap release rules the module teardown relies on: only a region start is released, once, a module's image
// and function table are separate regions, and loading a second image keeps the executable's region (patch 0085).
// Uses the real memory system; the refused releases log an error.
// Run: tmnt_heap_release_tests

#include <cstdint>
#include <cstdio>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

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

constexpr uint32_t kImageBase = 0x87500000;
constexpr uint32_t kImageSize = 0x1F0000;
constexpr uint32_t kCodeBase = 0x87520000;
constexpr uint32_t kCodeSize = 0x1BEE88;
constexpr uint32_t kPageSize = 0x10000;
constexpr uint32_t kReserveCommit = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
constexpr uint32_t kReadWrite = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;

bool AllocImage(rex::memory::BaseHeap* heap) {
  return heap->AllocFixed(kImageBase, kImageSize, 4096, kReserveCommit, kReadWrite);
}

void RegionStartIsAccepted(rex::memory::BaseHeap* heap) {
  CHECK(AllocImage(heap));
  uint32_t size = 0;
  CHECK(heap->Release(kImageBase, &size));
  CHECK(size >= kImageSize);
}

void InteriorPointerIsRejected(rex::memory::BaseHeap* heap) {
  CHECK(AllocImage(heap));
  CHECK(!heap->Release(kImageBase + kPageSize, nullptr));
  CHECK(!heap->Release(kImageBase + kImageSize - kPageSize, nullptr));
  CHECK(heap->Release(kImageBase, nullptr));
}

void SecondReleaseIsRejected(rex::memory::BaseHeap* heap) {
  CHECK(AllocImage(heap));
  CHECK(heap->Release(kImageBase, nullptr));
  CHECK(!heap->Release(kImageBase, nullptr));
}

void NeverAllocatedAddressIsRejected(rex::memory::BaseHeap* heap) {
  CHECK(!heap->Release(kImageBase, nullptr));
}

void FunctionTableIsItsOwnRegion(rex::memory::Memory& memory, rex::memory::BaseHeap* heap) {
  const uint32_t table_base = kImageBase + kImageSize;
  CHECK(AllocImage(heap));
  CHECK(memory.InitializeFunctionTable(kCodeBase, kCodeSize, kImageBase, kImageSize));
  CHECK(!heap->Release(table_base + kPageSize, nullptr));
  CHECK(memory.DestroyFunctionTable(kCodeBase));
  CHECK(!memory.DestroyFunctionTable(kCodeBase));
  CHECK(!heap->Release(table_base, nullptr));
  CHECK(heap->Release(kImageBase, nullptr));
}

void ImageSurvivesTheFunctionTableRelease(rex::memory::Memory& memory, rex::memory::BaseHeap* heap) {
  CHECK(AllocImage(heap));
  CHECK(memory.InitializeFunctionTable(kCodeBase, kCodeSize, kImageBase, kImageSize));
  CHECK(memory.DestroyFunctionTable(kCodeBase));
  CHECK(heap->Release(kImageBase, nullptr));
  CHECK(AllocImage(heap));
  CHECK(heap->Release(kImageBase, nullptr));
}

constexpr uint32_t kExeBase = 0x82000000;
constexpr uint32_t kExeSize = 0x1000000;
constexpr uint32_t kExeCodeBase = 0x82020000;
constexpr uint32_t kExeCodeSize = 0x800000;

// Setup reserves the executable's function table before either image is loaded.
void LoadExecutableThenModule(rex::memory::Memory& memory, rex::memory::BaseHeap* heap, bool reset_per_image) {
  CHECK(memory.InitializeFunctionTable(kExeCodeBase, kExeCodeSize, kExeBase, kExeSize));
  if (reset_per_image) heap->Reset();
  CHECK(heap->AllocFixed(kExeBase, kExeSize, 4096, kReserveCommit, kReadWrite));
  if (reset_per_image) heap->Reset();
  CHECK(AllocImage(heap));
  CHECK(memory.InitializeFunctionTable(kCodeBase, kCodeSize, kImageBase, kImageSize));
}

void ResettingTheHeapPerImageLosesTheExecutable(rex::memory::Memory& memory, rex::memory::BaseHeap* heap) {
  LoadExecutableThenModule(memory, heap, true);
  CHECK(memory.DestroyFunctionTable(kCodeBase));
  CHECK(heap->Release(kImageBase, nullptr));
  CHECK(!heap->Release(kExeBase, nullptr));
  CHECK(!heap->Release(kExeBase + kExeSize, nullptr));
  CHECK(memory.DestroyFunctionTable(kExeCodeBase));
}

void ExecutableImageIsReleasedExactlyOnce(rex::memory::Memory& memory, rex::memory::BaseHeap* heap) {
  LoadExecutableThenModule(memory, heap, false);
  CHECK(memory.DestroyFunctionTable(kCodeBase));
  CHECK(!memory.DestroyFunctionTable(kCodeBase));
  CHECK(heap->Release(kImageBase, nullptr));
  CHECK(!heap->Release(kImageBase, nullptr));
  CHECK(!heap->Release(kExeBase + kPageSize, nullptr));
  CHECK(heap->Release(kExeBase, nullptr));
  CHECK(!heap->Release(kExeBase, nullptr));
  CHECK(memory.DestroyFunctionTable(kExeCodeBase));
}

void ImageAllocatedAgainTakesOverItsRange(rex::memory::BaseHeap* heap) {
  CHECK(AllocImage(heap));
  CHECK(AllocImage(heap));
  CHECK(heap->Release(kImageBase, nullptr));
  CHECK(!heap->Release(kImageBase, nullptr));
}

}  // namespace

int main() {
  rex::InitLogging();
  rex::memory::Memory memory;
  if (!memory.Initialize()) {
    std::printf("Memory::Initialize failed\n");
    return 2;
  }
  rex::memory::BaseHeap* heap = memory.LookupHeap(kImageBase);
  if (!heap) {
    std::printf("no heap at %08X\n", kImageBase);
    return 2;
  }

  RegionStartIsAccepted(heap);
  InteriorPointerIsRejected(heap);
  SecondReleaseIsRejected(heap);
  NeverAllocatedAddressIsRejected(heap);
  FunctionTableIsItsOwnRegion(memory, heap);
  ImageSurvivesTheFunctionTableRelease(memory, heap);
  ResettingTheHeapPerImageLosesTheExecutable(memory, heap);
  ExecutableImageIsReleasedExactlyOnce(memory, heap);
  ImageAllocatedAgainTakesOverItsRange(heap);

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  std::printf(g_failures ? "FAIL\n" : "PASS\n");
  return g_failures ? 1 : 0;
}
