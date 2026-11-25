# Resultados do A/B: custo de páginas por laço em index scan parametrizado

Medição: build com o experimento de custo (fórmula por laço em `cost_index()`
e `compute_bitmap_pages()`) sobre a pilha #5–#9, contra `master`. Queries
aquecidas, `statement_timeout=100s`, builds trocados entre as rodadas.
Custo é o `Total Cost` do plano; tempo é `Execution Time`.

Planos alterados: **88** de 121 queries. Mudanças
significativas (≥50 ms e ≥20%): **15 melhoras, 5 pioras**.

## Melhoras

| query | custo (antes → depois) | tempo (antes → depois) | o que mudou |
|---|---|---|---|
| DS72 | 30,440 → 42,603 | 11,820 → **180 ms** | Materialize 1→0 |
| H07 | 73,078 → 195,923 | 2,311 → **235 ms** | Index Scan 4→0; Seq Scan 2→6; Hash Join 2→5 |
| DS78 | 96,049 → 100,146 | 2,283 → **1,278 ms** | Index Scan 6→5; Index Only Scan 2→3 |
| H21 | 73,823 → 296,198 | 1,548 → **562 ms** | Hash Join 2→3; Nested Loop 3→2; Index Scan 3→2 |
| DS60 | 69,732 → 109,022 | 983 → **141 ms** | Hash Join 5→7; Index Scan 7→5; Nested Loop 7→5 |
| DS21 | 85,961 → 89,293 | 400 → **56 ms** | Hash Join 1→2; Gather Merge 0→1; Nested Loop 2→1 |
| DS18 | 27,230 → 34,067 | 456 → **137 ms** | Hash Join 0→2; Nested Loop 6→4; Index Scan 5→3 |
| DS56 | 32,605 → 108,410 | 350 → **123 ms** | Hash Join 4→7; Index Scan 8→5; Nested Loop 8→5 |
| DS38 | 128,471 → 137,908 | 830 → **624 ms** | Hash Join 2→5; Index Scan 4→1; Nested Loop 4→1 |
| DS87 | 128,807 → 138,237 | 810 → **624 ms** | Hash Join 2→5; Index Scan 4→1; Nested Loop 4→1 |
| DS36 | 73,444 → 73,281 | 444 → **259 ms** | Hash Join 1→2; Index Scan 2→1; Nested Loop 2→1 |
| H05 | 92,147 → 186,364 | 430 → **251 ms** | Hash Join 3→5; Nested Loop 2→0; Index Scan 2→0 |
| DS77 | 114,283 → 118,224 | 279 → **172 ms** | Hash Join 2→5; Hash 2→5; Index Scan 4→2 |
| DS79 | 74,079 → 73,717 | 158 → **100 ms** | Hash Join 2→3; Nested Loop 2→1; Index Scan 2→1 |
| DS53 | 14,381 → 16,246 | 231 → **179 ms** | Hash Join 0→1; Index Scan 1→0; Index Only Scan 0→1 |

## Pioras

| query | custo (antes → depois) | tempo (antes → depois) | o que mudou |
|---|---|---|---|
| DS10 | 888,610 → 946,048 | 32 → **116 ms** | Hash Join 0→2; Nested Loop 6→4; Index Scan 8→6 |
| DS07 | 72,708 → 87,650 | 316 → **481 ms** | Index Scan 3→1; Seq Scan 2→4; Hash Join 1→2 |
| DS69 | 73,152 → 94,792 | 115 → **372 ms** | Index Scan 7→2; Seq Scan 2→7; Hash Join 1→4 |
| H04 | 82,807 → 190,137 | 148 → **764 ms** | Hash Join 0→1; Nested Loop 1→0; Index Scan 1→0 |
| H09 | 50,875 → 163,020 | 1,453 → **2,374 ms** | Bitmap Index Scan 0→2; BitmapAnd 0→1; Index Scan 3→2 |

## Leitura

Nas melhoras, o padrão é o mesmo: o plano antigo fazia nested loop com index
scan parametrizado (ou bitmap) reexecutado milhares de vezes, e o custo
estimado era baixo justamente porque as páginas eram amortizadas entre os
laços. Com o custo por laço, o planner troca por hash joins. O custo estimado
do plano novo é **maior** que o do antigo (DS72: 30.440 → 42.603), mas o tempo
real cai de 11,8 s para 0,18 s: o custo antigo era uma ficção.

Nas pioras, o mesmo modelo cobra demais quando há poucos laços e as páginas
realmente se reaproveitam: H04 (148 → 764 ms) e H09 (1,45 → 2,37 s) trocam NL
por hash join e perdem; DS69 (115 → 372 ms) idem. A fórmula por laço precisa
ser condicionada ao número de laços e à fração da relação tocada — não
aplicada sempre.

## Planos completos

Formas dos planos (tipos de nó, da raiz para as folhas):


**DS72**

- antes: `Limit>Sort>Aggregate>Sort>Gather>Nested Loop>Nested Loop>Nested Loop>Nested Loop>Hash Join>Nested Loop>Nested Loop>Hash Join>Nested Loop>Seq Scan>Index Scan>Hash>Seq Scan>Index Scan>Index Scan>Hash>Seq Scan>Materialize>Seq Scan>Index Scan>Index Scan>Index Only Scan`
- depois: `Limit>Sort>Aggregate>Sort>Nested Loop>Nested Loop>Nested Loop>Nested Loop>Gather>Nested Loop>Hash Join>Nested Loop>Hash Join>Nested Loop>Seq Scan>Index Scan>Hash>Seq Scan>Index Scan>Hash>Seq Scan>Index Scan>Seq Scan>Index Scan>Index Scan>Index Only Scan`

**H07**

- antes: `Aggregate>Gather Merge>Sort>Hash Join>Nested Loop>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Hash>Nested Loop>Nested Loop>Index Scan>Memoize>Index Scan>Index Scan`
- depois: `Aggregate>Gather Merge>Sort>Hash Join>Hash Join>Seq Scan>Hash>Hash Join>Seq Scan>Hash>Hash Join>Seq Scan>Hash>Seq Scan>Hash>Hash Join>Seq Scan>Hash>Seq Scan`

**DS78**

- antes: `Limit>Incremental Sort>Merge Join>Merge Join>Aggregate>Incremental Sort>Gather Merge>Nested Loop>Nested Loop>Index Scan>Memoize>Index Scan>Index Only Scan>Materialize>Aggregate>Gather Merge>Aggregate>Incremental Sort>Nested Loop>Nested Loop>Index Scan>Memoize>Index Scan>Index Only Scan>Sort>Subquery Scan>Aggregate>Gather Merge>Sort>Nested Loop>Nested Loop>Seq Scan>Index Scan>Index Scan`
- depois: `Limit>Incremental Sort>Merge Join>Merge Join>Aggregate>Incremental Sort>Gather Merge>Nested Loop>Nested Loop>Index Scan>Memoize>Index Scan>Index Only Scan>Materialize>Aggregate>Gather Merge>Aggregate>Incremental Sort>Nested Loop>Nested Loop>Index Scan>Memoize>Index Scan>Index Only Scan>Sort>Subquery Scan>Aggregate>Gather Merge>Sort>Nested Loop>Nested Loop>Seq Scan>Index Scan>Index Only Scan`

**H21**

- antes: `Sort>Aggregate>Sort>Nested Loop>Gather>Nested Loop>Hash Join>Seq Scan>Hash>Nested Loop>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Index Scan>Index Scan`
- depois: `Sort>Aggregate>Sort>Nested Loop>Nested Loop>Gather>Hash Join>Seq Scan>Hash>Hash Join>Seq Scan>Hash>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Index Scan`

**DS60**

- antes: `Limit>Incremental Sort>Aggregate>Merge Append>Aggregate>Gather Merge>Sort>Nested Loop>Hash Join>Nested Loop>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Hash>Seq Scan>Index Scan>Aggregate>Gather Merge>Sort>Nested Loop>Hash Join>Nested Loop>Nested Loop>Seq Scan>Index Scan>Index Scan>Hash>Seq Scan>Index Scan>Aggregate>Gather Merge>Sort>Nested Loop>Hash Join>Nested Loop>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Hash>Seq Scan>Index Scan`
- depois: `Limit>Incremental Sort>Aggregate>Merge Append>Aggregate>Gather Merge>Sort>Nested Loop>Hash Join>Hash Join>Seq Scan>Hash>Seq Scan>Hash>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Aggregate>Gather Merge>Sort>Nested Loop>Hash Join>Nested Loop>Seq Scan>Index Scan>Hash>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Aggregate>Gather Merge>Aggregate>Sort>Nested Loop>Hash Join>Nested Loop>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Hash>Seq Scan>Index Scan`

**DS21**

- antes: `Limit>Aggregate>Sort>Hash Join>Gather>Nested Loop>Nested Loop>Seq Scan>Index Scan>Index Scan>Hash>Seq Scan`
- depois: `Limit>Aggregate>Gather Merge>Sort>Hash Join>Hash Join>Nested Loop>Seq Scan>Index Scan>Hash>Seq Scan>Hash>Seq Scan`

**DS10**

- antes: `Limit>Aggregate>Sort>Nested Loop>Nested Loop>Nested Loop>Seq Scan>Index Scan>Nested Loop>Index Scan>Index Scan>Nested Loop>Index Scan>Index Scan>Nested Loop>Index Scan>Index Scan>Index Scan`
- depois: `Limit>Aggregate>Sort>Nested Loop>Hash Join>Nested Loop>Seq Scan>Index Scan>Nested Loop>Index Scan>Index Scan>Nested Loop>Index Scan>Index Scan>Hash>Gather>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan`

**DS07**

- antes: `Limit>Aggregate>Gather Merge>Aggregate>Sort>Nested Loop>Nested Loop>Nested Loop>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Index Scan>Index Scan`
- depois: `Limit>Aggregate>Nested Loop>Gather Merge>Sort>Seq Scan>Materialize>Gather>Nested Loop>Hash Join>Hash Join>Seq Scan>Hash>Seq Scan>Hash>Seq Scan>Index Scan`

**DS69**

- antes: `Limit>Aggregate>Nested Loop>Nested Loop>Nested Loop>Gather Merge>Sort>Nested Loop>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Nested Loop>Index Scan>Index Scan>Nested Loop>Index Scan>Index Scan>Nested Loop>Index Scan>Index Scan`
- depois: `Limit>Aggregate>Nested Loop>Nested Loop>Gather Merge>Sort>Nested Loop>Hash Join>Hash Join>Seq Scan>Hash>Seq Scan>Hash>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Materialize>Gather>Hash Join>Seq Scan>Hash>Seq Scan>Materialize>Gather>Nested Loop>Seq Scan>Index Scan`

**H04**

- antes: `Aggregate>Gather Merge>Aggregate>Sort>Nested Loop>Bitmap Heap Scan>Bitmap Index Scan>Index Scan`
- depois: `Aggregate>Gather Merge>Aggregate>Sort>Hash Join>Bitmap Heap Scan>Bitmap Index Scan>Hash>Seq Scan`

**H09**

- antes: `Aggregate>Gather Merge>Aggregate>Sort>Hash Join>Nested Loop>Nested Loop>Nested Loop>Hash Join>Seq Scan>Hash>Seq Scan>Index Scan>Index Scan>Index Scan>Hash>Seq Scan`
- depois: `Aggregate>Gather Merge>Aggregate>Sort>Hash Join>Nested Loop>Nested Loop>Nested Loop>Hash Join>Seq Scan>Hash>Seq Scan>Bitmap Heap Scan>BitmapAnd>Bitmap Index Scan>Bitmap Index Scan>Index Scan>Index Scan>Hash>Seq Scan`
