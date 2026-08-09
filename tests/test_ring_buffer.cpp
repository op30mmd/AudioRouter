#include "../src/common/ring_buffer.hpp"
#include <iostream>
#include <vector>
#include "../src/common/span_compat.hpp"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool run_ring_buffer_tests() {
    using namespace audiorouter;

    RingBuffer<int16_t> ring(100);
    TEST_ASSERT(ring.capacity() == 100);
    TEST_ASSERT(ring.size() == 0);
    TEST_ASSERT(ring.empty());
    TEST_ASSERT(!ring.full());

    std::vector<int16_t> in_data(40);
    for (int i = 0; i < 40; ++i) in_data[i] = static_cast<int16_t>(i + 1);

    size_t written = ring.write(in_data.data(), in_data.size());
    TEST_ASSERT(written == 40);
    // span overload
    TEST_ASSERT(ring.free_space() == 60);
    TEST_ASSERT(ring.write(std::span<const int16_t>{})==0);

    std::vector<int16_t> out_data(20);
    size_t read_cnt = ring.read(out_data.data(), 20);
    TEST_ASSERT(read_cnt == 20);
    TEST_ASSERT(ring.size() == 20);
    for (int i = 0; i < 20; ++i) TEST_ASSERT(out_data[i] == static_cast<int16_t>(i + 1));

    // wrap-around write/read via span
    std::vector<int16_t> more_data(70);
    for (int i = 0; i < 70; ++i) more_data[i] = static_cast<int16_t>(100 + i);
    written = ring.write(std::span<const int16_t>(more_data));
    TEST_ASSERT(written == 70);
    TEST_ASSERT(ring.size() == 90);

    // peek without removing
    {
        std::vector<int16_t> peek(10);
        size_t p = ring.peek(peek.data(), 10);
        TEST_ASSERT(p==10);
        TEST_ASSERT(peek[0]==21); // first of remaining 21..40
        TEST_ASSERT(ring.size()==90); // still 90
        std::vector<int16_t> peek_span(5);
        TEST_ASSERT(ring.peek(std::span<int16_t>(peek_span))==5);
    }

    // skip
    {
        size_t skipped = ring.skip(5);
        TEST_ASSERT(skipped==5 && ring.size()==85);
        // skip beyond size
        RingBuffer<int> r2(10);
        r2.write(std::vector<int>{1,2,3}.data(),3);
        TEST_ASSERT(r2.skip(10)==3 && r2.empty());
    }

    // read_pad_silence
    {
        RingBuffer<int16_t> r(10);
        r.write(std::vector<int16_t>{1,2,3}.data(),3);
        std::vector<int16_t> padded(5, 99);
        r.read_pad_silence(padded.data(), 5, int16_t(0));
        TEST_ASSERT(padded[0]==1 && padded[2]==3 && padded[3]==0 && padded[4]==0);
        // span overload
        r.clear();
        r.write(std::vector<int16_t>{7,8}.data(),2);
        std::vector<int16_t> span_padded(4, 5);
        r.read_pad_silence(std::span<int16_t>(span_padded), int16_t(-1));
        TEST_ASSERT(span_padded[0]==7 && span_padded[2]==-1);
    }

    // read remaining + wrap
    std::vector<int16_t> all_read(85);
    read_cnt = ring.read(all_read.data(), 85);
    TEST_ASSERT(read_cnt == 85);
    TEST_ASSERT(ring.empty());
    // after skip of 5, the first element originally 21..40 had first 5 skipped, so remaining 26..40 + 100..169 but note we consumed 5, so check
    TEST_ASSERT(all_read[0]==26);

    // overwrite with vector larger than capacity
    ring.clear();
    std::vector<int16_t> big_data(120);
    for (int i = 0; i < 120; ++i) big_data[i] = static_cast<int16_t>(i);
    ring.write_overwrite(big_data.data(), big_data.size());
    TEST_ASSERT(ring.full() && ring.size()==100);
    std::vector<int16_t> read_big(100);
    ring.read(read_big.data(), 100);
    TEST_ASSERT(read_big[0]==20 && read_big[99]==119);

    // overwrite with span smaller than capacity but needing drop
    ring.clear();
    ring.write(std::vector<int16_t>{1,2,3,4,5,6,7,8}.data(),8); // 8
    // capacity 100, free 92, write 95 via overwrite -> should drop 3
    std::vector<int16_t> over(95, 9);
    over[0]=100;
    ring.write_overwrite(over.data(), over.size());
    TEST_ASSERT(ring.size()==100);
    // try_write should fail when full
    {
        auto res = ring.try_write(std::span<const int16_t>(std::vector<int16_t>{1}));
        TEST_ASSERT(!res.has_value());
        ring.clear();
        auto ok = ring.try_write(std::span<const int16_t>(std::vector<int16_t>{1,2}));
        TEST_ASSERT(ok.has_value() && *ok==2);
    }

    // null and zero handling
    TEST_ASSERT(ring.write(nullptr, 10)==0);
    TEST_ASSERT(ring.read(nullptr, 10)==0);
    TEST_ASSERT(ring.write(std::span<const int16_t>{})==0);
    // resize
    RingBuffer<int> r3(10);
    r3.write(std::vector<int>{1,2,3}.data(),3);
    r3.resize(5);
    TEST_ASSERT(r3.capacity()==5 && r3.empty()); // resize clears

    // move semantics
    RingBuffer<int> a(10);
    a.write(std::vector<int>{1,2}.data(),2);
    RingBuffer<int> b(std::move(a));
    TEST_ASSERT(b.size()==2);

    // generic type test (float)
    RingBuffer<float> rf(4);
    rf.write(std::vector<float>{1.1f,2.2f}.data(),2);
    std::vector<float> rout(2);
    rf.read(rout.data(),2);
    TEST_ASSERT(rout[0]==1.1f);

    // highly fragmented wrap: fill, partial read, fill again to wrap head/tail
    RingBuffer<uint8_t> wrap(8);
    wrap.write(std::vector<uint8_t>{0,1,2,3,4,5}.data(),6);
    std::vector<uint8_t> tmp(4);
    wrap.read(tmp.data(),4); // head 6 tail 4 count 2
    wrap.write(std::vector<uint8_t>{6,7,8,9,10}.data(),5); // should wrap
    TEST_ASSERT(wrap.size()==7);
    std::vector<uint8_t> all(7);
    wrap.read(all.data(),7);
    TEST_ASSERT(all[0]==4 && all[1]==5 && all[2]==6); // check order preserved across wrap

    return true;
}
