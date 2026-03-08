## Бенчмарк для вычисления характеристик L1 кеша

#### Сборка и запуск

```
clang++ -std=c++17 -O2 main.cpp -o main && ./main
```

Результаты для моего Apple M1 Pro:

```
...
L1 size: 64 KB
L1 associativity: 8
L1 cache line size: 128 B
```

Результаты для кафедрального сервера ВТ ИТМО Helios (Intel(R) Xeon(R) CPU E5-2643 0 @ 3.30GHz):

```
....
L1 size: 32 KB
L1 associativity: 8
L1 cache line size: 64
```