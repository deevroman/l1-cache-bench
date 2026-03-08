#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <vector>

using namespace std;

constexpr size_t INITIAL_CACHE_LINE = 64;
constexpr size_t MAX_MEMORY = 512 * 1024;
constexpr size_t ITERATIONS = 1024 * 1024 * 32;

class ScopedBuffer
{
public:
    explicit ScopedBuffer(size_t size) : size_(size)
    {
        if (posix_memalign(reinterpret_cast<void**>(&data_), INITIAL_CACHE_LINE, size_))
        {
            cerr << "alloc failed\n";
            exit(1);
        }
        clear();
    }

    ~ScopedBuffer()
    {
        free(data_);
    }

    ScopedBuffer(const ScopedBuffer&) = delete;
    ScopedBuffer& operator=(const ScopedBuffer&) = delete;

    uint8_t* data() const
    {
        return data_;
    }

    size_t size() const
    {
        return size_;
    }

    void clear() const
    {
        fill_n(data_, size_, 0);
    }

private:
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

void build_chain(uint8_t* buffer, const size_t stride, const size_t spots)
{
    vector<size_t> perm(spots);
    for (size_t i = 0; i < spots; ++i)
    {
        perm[i] = i;
    }

    mt19937_64 rng(123456); // NOLINT
    for (size_t i = spots - 1; i > 0; --i)
    {
        uniform_int_distribution<size_t> dist(0, i);
        size_t j = dist(rng);
        swap(perm[i], perm[j]);
    }

    for (size_t i = 0; i < spots; ++i)
    {
        const size_t cur = perm[i];
        const size_t next = perm[(i + 1) % spots];

        const auto slot = reinterpret_cast<uint8_t**>(buffer + cur * stride);
        *slot = buffer + next * stride;
    }
}

// на всякий случай проверка, что цепочка правильно сгенерирована
// побочный эффект — прогрев кешей
void verify_chain(uint8_t* buffer, size_t stride, size_t spots)
{
    vector seen(spots, false);

    uint8_t* p = buffer;
    for (size_t i = 0; i < spots; ++i)
    {
        const size_t index = (p - buffer) / stride;
        if (index >= spots || seen[index])
        {
            cerr << "Broken chain\n";
            abort();
        }
        seen[index] = true;
        p = *reinterpret_cast<uint8_t**>(p);
    }
}

struct RunResult
{
    uint8_t* ptr;
    uint64_t ns;
};

RunResult run(uint8_t* buffer, size_t iterations = ITERATIONS)
{
    uint8_t* p = buffer;
    const auto start = chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i)
    {
        p = *reinterpret_cast<uint8_t**>(p);
    }
    const auto end = chrono::steady_clock::now();
    const uint64_t ns = chrono::duration_cast<chrono::nanoseconds>(end - start).count();
    return RunResult{p, ns};
}

struct Sample
{
    size_t stride;
    size_t spots;
    double latency_ns;
};


uint64_t median_value(vector<uint64_t>& values)
{
    const size_t mid = values.size() / 2;
    nth_element(values.begin(), values.begin() + mid, values.end());
    return values[mid];
}

double measure_latency(uint8_t* buffer, size_t iterations, size_t runs)
{
    vector<uint64_t> times;
    times.reserve(runs);

    uintptr_t acc = 0;
    for (size_t i = 0; i < runs; ++i)
    {
        const auto [ptr, ns] = run(buffer, iterations);
        acc ^= reinterpret_cast<uintptr_t>(ptr);
        times.push_back(ns);
    }
    // защита от оптимизаций компилятора
    cout << "";
    if (acc == 0xdeadbeef)
    {
        cout << acc;
    }
    return static_cast<double>(median_value(times)) / iterations;
}

size_t detect_l1_size(const vector<Sample>& samples, size_t stride)
{
    vector<const Sample*> row;
    for (const auto& s : samples)
    {
        if (s.stride == stride)
        {
            row.push_back(&s);
        }
    }

    if (row.size() < 6)
    {
        return 0;
    }

    vector<double> base;
    for (size_t i = 0; i < 3; ++i)
    {
        base.push_back(row[i]->latency_ns);
    }

    sort(base.begin(), base.end());
    const double baseline = base[1];

    for (size_t i = 3; i + 2 < row.size(); ++i)
    {
        if (row[i]->latency_ns > baseline * 1.5
            && row[i + 1]->latency_ns > baseline * 1.5
            && row[i + 2]->latency_ns > baseline * 1.5)
        {
            return row[i - 1]->stride * row[i - 1]->spots;
        }
    }

    return 0;
}

// Определение размера кеша:
// Строим цепочки указателей перебирая параметры цепочки: расстояние между указтелями и её длину
// и ищем первый скачок времени при итерировании по цепочке
size_t run_cache_size_benchmark()
{
    ScopedBuffer buffer(MAX_MEMORY);
    vector<Sample> samples;
    vector<size_t> l1_estimates;

    cout << "stride \tspots \tns\n";

    for (size_t stride = INITIAL_CACHE_LINE; stride <= 4 * 1024; stride *= 2)
    {
        for (size_t spots = 1; spots * stride <= MAX_MEMORY; spots *= 2)
        {
            build_chain(buffer.data(), stride, spots);
            verify_chain(buffer.data(), stride, spots);

            const double latency = measure_latency(buffer.data(), ITERATIONS, 7);

            samples.push_back(Sample{
                stride,
                spots,
                latency
            });

            cout << setw(4) << stride << "\t"
                << setw(4) << spots << "\t"
                << fixed << setprecision(2)
                << latency << " ns\n";
        }

        buffer.clear();
        cout << "\n";
    }

    cout << "\nL1 size:\n";
    for (size_t stride = INITIAL_CACHE_LINE; stride <= 4 * 1024; stride *= 2)
    {
        if (const auto l1 = detect_l1_size(samples, stride))
        {
            cout << "stride " << stride << ": L1 = " << (l1 / 1024) << " KB\n";
            l1_estimates.push_back(l1);
        }
    }

    if (l1_estimates.empty())
    {
        return 0;
    }

    map<size_t, size_t> freq;
    for (size_t value : l1_estimates)
    {
        ++freq[value];
    }

    return max_element(freq.begin(), freq.end(),
                       [](const auto& a, const auto& b)
                       {
                           return a.second < b.second;
                       })->first;
}

size_t build_assoc_conflict_chain(uint8_t* buffer, size_t l1_size, size_t assoc)
{
    const size_t block_interval = l1_size / sizeof(uint8_t*);
    const size_t block_words = block_interval / assoc;

    auto** slots = reinterpret_cast<uint8_t**>(buffer);
    bool first = true;
    size_t first_index = 0;
    size_t prev_index = 0;

    for (size_t word_idx = 0; word_idx < block_words; ++word_idx)
    {
        for (size_t way_idx = 0; way_idx < assoc; ++way_idx)
        {
            const size_t idx = way_idx * block_interval + word_idx;
            if (first)
            {
                first = false;
                first_index = idx;
                prev_index = idx;
            }
            else
            {
                slots[prev_index] = reinterpret_cast<uint8_t*>(slots + idx);
                prev_index = idx;
            }
        }
    }
    slots[prev_index] = reinterpret_cast<uint8_t*>(slots + first_index);
    return block_words * assoc;
}

void verify_assoc_chain(uint8_t* buffer, size_t length)
{
    uint8_t* p = buffer;
    for (size_t i = 0; i < length; ++i)
    {
        p = *reinterpret_cast<uint8_t**>(p);
    }

    if (p != buffer)
    {
        cerr << "Broken associativity chain\n";
        abort();
    }
}

// Определение ассоциотивности:
// перебираем значение ассоциативности кеша и строим зацикленные цепочки так,
// чтобы последовательные указатели оказывались в разных наборах
// из-за чего при превышения значения мы увидим скачок во времени
size_t run_associativity_benchmark(size_t l1_size)
{
    constexpr size_t MAX_ASSOC = 32;
    ScopedBuffer buffer(l1_size * MAX_ASSOC);

    struct AssocSample
    {
        size_t assoc;
        double latency_ns;
    };

    vector<AssocSample> samples;
    samples.reserve(MAX_ASSOC);

    cout << "\nassociativity \tns\n";

    for (size_t assoc = 1; assoc <= MAX_ASSOC; ++assoc)
    {
        auto length = build_assoc_conflict_chain(buffer.data(), l1_size, assoc);
        verify_assoc_chain(buffer.data(), length);

        const double latency = measure_latency(buffer.data(), ITERATIONS, 7);
        samples.push_back(AssocSample{assoc, latency});

        cout << setw(12) << assoc << "\t"
            << fixed << setprecision(2)
            << latency << " ns\n";
    }

    const double baseline = min({samples[0].latency_ns, samples[1].latency_ns, samples[2].latency_ns});
    size_t lower_step_assoc = 0;
    bool step_is_lower = false;

    for (const auto& s : samples)
    {
        if (step_is_lower && s.latency_ns >= baseline * 1.5)
        {
            return lower_step_assoc;
        }

        if (s.latency_ns <= baseline * 1.35)
        {
            step_is_lower = true;
            lower_step_assoc = s.assoc;
        }
        else
        {
            step_is_lower = false;
        }
    }

    return 0;
}


void build_linear_stride_chain(uint8_t* buffer, size_t stride, size_t spots)
{
    for (size_t i = 0; i < spots; ++i)
    {
        const auto slot = reinterpret_cast<uint8_t**>(buffer + i * stride);
        const size_t next = (i + 1) % spots;
        *slot = buffer + next * stride;
    }
}

// Определение длины линеек
// Строим цепочки с шагом в потенциальный размер линейки
// и ищем скачок при переборе размера шага
size_t run_cache_line_benchmark()
{
    constexpr size_t MIN_LINE = sizeof(uint8_t*) * 2;
    constexpr size_t MAX_LINE = 256;

    ScopedBuffer buffer(8 * 1024 * 1024);

    struct LineSample
    {
        size_t line_size;
        double latency_ns;
    };

    vector<LineSample> samples;

    cout << "\nline-size \tns\n";

    for (size_t line_size = MIN_LINE; line_size <= MAX_LINE; line_size *= 2)
    {
        const size_t spots = buffer.size() / line_size;
        if (spots < 2)
        {
            continue;
        }

        build_linear_stride_chain(buffer.data(), line_size, spots);
        verify_chain(buffer.data(), line_size, spots);

        const double latency = measure_latency(buffer.data(), ITERATIONS * 2, 11);
        samples.push_back(LineSample{line_size, latency});

        cout << setw(8) << line_size << "\t"
            << fixed << setprecision(4)
            << latency << " ns\n";
    }

    for (size_t i = 1; i + 1 < samples.size(); ++i)
    {
        const double a = samples[i].latency_ns / samples[i - 1].latency_ns;
        const double b = samples[i + 1].latency_ns / samples[i].latency_ns;
        if (a >= 1.45 && b >= 1.05)
        {
            return samples[i].line_size;
        }
    }

    size_t best_i = 2;
    double best_ratio = 0.0;
    for (size_t i = 2; i + 1 < samples.size(); ++i)
    {
        const double ratio = samples[i].latency_ns / samples[i - 1].latency_ns;
        if (ratio > best_ratio)
        {
            best_ratio = ratio;
            best_i = i;
        }
    }
    return samples[best_i - 1].line_size;
}

int main()
{
    const size_t l1_size = run_cache_size_benchmark();

    if (l1_size == 0)
    {
        cout << "\nFailed to determine L1 cache size reliably.\n";
        return 1;
    }

    const size_t assoc = run_associativity_benchmark(l1_size);
    const size_t line_size = run_cache_line_benchmark();

    cout << "\nResult:\n"
        << "L1 size: " << l1_size / 1024 << " KB\n"
        << "L1 associativity: " << assoc << "\n"
        << "L1 cache line size: " << line_size << " B\n";

    return 0;
}
