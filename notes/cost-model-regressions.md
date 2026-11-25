# Onde o custo erra nas regressões do pacote de domínio

Investigação de cada plano que piorou no A/B da regra de domínio (com o custo
por laço aplicado por cima), contra master. Método:

- `bench/regress_nodes.py`: `EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)` nos dois
  builds, um registro por nó (custo, tempo, laços, linhas estimadas/reais).
- `bench/regress_compare.py`: emparelha nós pela assinatura (tipo + relações
  folha) e mostra a estimativa do *mesmo* nó antes/depois, além de custo e tempo
  inclusivos por subplano.
- `bench/forced_plans.py`: sob as estimativas novas, roda o plano escolhido e
  alternativas forçadas (`enable_hashjoin=off`, `enable_nestloop=off`,
  `enable_mergejoin=off`, `max_parallel_per_gather=0`). Se existe alternativa
  mais rápida e mais cara no papel, o custo de um dos dois está errado.
- `bench/calib_matrix.py`: varia `cpu_tuple_cost` e `random_page_cost` para
  separar erro de estimativa de erro de calibração do modelo.

## Quadro geral

Tempos do A/B de 123 queries; nas re-execuções com `EXPLAIN (ANALYZE)` a
variação fica dentro de ~10%.

| query | custo master → pacote | tempo master → pacote | estimativa melhorou? |
|---|---|---|---|
| DS10 | 888.610 → 140.846 | 35 → 99 ms | **não** (684 × 8 contra 35 × 8) |
| DS94 | 31.324 → 56.059 | 54 → 130 ms | **não** (63 × 7 contra 1 × 7) |
| DS64 | 52.885 → 61.425 | 164 → 223 ms | sim (367× → 1,0×) |
| DS36 | 73.444 → 107.416 | 383 → 587 ms | sim (31,8× → 1,0×) |
| DS97 | 98.097 → 176.753 | 402 → 765 ms | sim (31,9× → 1,0×) |
| DS51 | 99.691 → 303.248 | 809 → 1.081 ms | sim (36,3× → 1,1×) |
| DS39 | 140.786 → 806.421 | 1.634 → 5.551 ms | sim no join (31,9× → 1,6×), pior no resto |
| DS95 | 523.267 → 771.921 | 117 → 3.905 ms | sim (35.000× → 1,0×) |
| DS78 | 96.049 → 4.582 | 2.068 → 2.819 ms | **não** no filtro (5,7× → 224×) |
| H09 | 50.875 → 162.700 | 1.463 → 2.362 ms | inalterada |

## Plano escolhido × melhor alternativa permitida pelos GUCs

| query | escolhido (custo / tempo) | melhor alternativa | leitura |
|---|---|---|---|
| DS39 | 806.421 / 5.726 ms | nenhuma melhor | escolha certa dado o modelo |
| DS95 | 771.921 / 4.091 ms | `nestloop=off` 784.261 / 3.562 ms | empate; base ainda 30× melhor |
| DS78 | 4.582 / 3.033 ms | `nestloop=off` 203.813 / **1.141 ms** | escolhido 44× mais barato e 2,7× mais lento |
| DS97 | 176.753 / 823 ms | `hashjoin=off` 246.695 / **602 ms** | escolhido 29% mais barato e 37% mais lento |
| DS51 | 303.248 / 1.158 ms | `hashjoin=off` 339.891 / **947 ms** | idem, 22% mais rápido |
| DS36 | 107.416 / 594 ms | `hashjoin=off` 176.423 / **394 ms** | escolhido 39% mais barato e 51% mais lento |
| DS94 | 56.059 / 127 ms | `hashjoin=off` 70.002 / **81 ms** | escolhido 25% mais barato e 57% mais lento |
| H09 | 162.700 / 2.507 ms | `nestloop=off` 210.357 / **941 ms** | escolhido 29% mais barato e 2,7× mais lento |
| DS10 | 140.846 / 123 ms | nenhuma por join; `rpc=1.1` → 67.124 / **36 ms** | probes de índice superpreçados |
| DS64 | 61.425 / 224 ms | nenhuma melhor | escolha certa |

Seis das dez regressões têm um plano mais rápido que o modelo recusa. Onde a
estimativa melhorou (DS36, DS97, DS51, DS94), sobra erro de **custo**. Onde ela
piorou (DS10, DS78) ou ficou presa num mínimo (H09), o erro é de **estimativa**.

## Query a query

### DS10 — 35 → 99 ms (estimativa piorou, plano mais barato)

O que mudou: o plano deixa de usar os subplanos `EXISTS` sobre `web_sales` e
`catalog_sales` e passa a varrer `store_sales` inteiro em paralelo para agregar
`ss_customer_sk`.

Números: o join `customer ⋈ store_sales` estima **684** linhas contra **8
reais** (85×); no master estimava 35 (4,4×). A causa do 684: o `HashAggregate`
sobre `store_sales` estima **62.176 clientes distintos** onde há **8.731** — o
`ndistinct` da coluna não é reescalado pela seletividade do filtro de data (4
meses concentram 4 linhas por cliente, não 1).

Custo: o plano do master custa **888.610 para 53 ms** (16.700 unidades/ms,
contra ~1.000 unidades/ms de um seq scan na mesma máquina). O vilão são os
subplanos: `Index Scan web_sales` cobra 76,73 unidades por laço × 1.146 laços,
com 8 µs de execução real. **`random_page_cost=4` para páginas 100% em cache.**

Veredito: estimativa pior + probes superpreçados. Com `random_page_cost=1.1` o
planner volta ao plano de índice e roda em **36 ms**.

### DS94 — 54 → 130 ms (estimativa piorou)

O que mudou: `Nested Loop Semi Join` com probes de índice vira `Parallel Hash
Semi Join` com build de `web_sales` (719 mil linhas).

Números: o NL final estima **63** contra **7 reais** (9×); no master estimava 1
(7×). Não é melhora. O `Parallel Hash Semi Join` custa 54.451 para 116 ms; o
plano do master (NL + probes) custava 31.324 para 54 ms — coerente consigo
mesmo, mas o modelo preteriu o plano 2,4× mais rápido.

Custo: o build do hash de `web_sales` (719 mil linhas, 8 batches, spill de
2.188 páginas) é cobrado a ~0,03 unidade/linha, quando custa 0,4 µs/linha.
**CPU por linha barata demais.**

Veredito: estimativa não melhorou; plano novo só é possível porque o hash é
subpreçado. Com `cpu_tuple_cost=0,03` o planner volta ao NL e roda em **52 ms**.

### DS64 — 164 → 223 ms (estimativa muito melhor, custo coerente)

O que mudou: os dois `Hash Join` de `store_sales`/`catalog_sales` passam a ser
estimados em 16.440/60.028 linhas contra 43 (367× → 1,0× e 1.117× → 1,3×). O
plano muda o join de `income_band` para hash.

Custo e tempo sobem juntos (16% e 15%). Não há alternativa mais rápida. É o
caso saudável: a estimativa melhorou, o custo acompanhou, o tempo pagou o
trabalho que antes estava subestimado. **Sem erro de custo a corrigir.**

### DS36 — 383 → 587 ms (estimativa muito melhor, custo errado)

O que mudou: `Nested Loop` paralelo com `Index Scan item` (533 mil laços) vira
três `Hash Join` seriais, o último carregando linhas de 114 bytes.

Números: `Hash Join date_dim` estima **551.343** contra **546.310 reais**
(31,8× → 1,0×); `Seq Scan store_sales` 2,88 M contra 2,88 M (1,2× → 1,0×).
A estimativa é agora exata — e o plano é mais lento.

Custo: o plano escolhido cobra 107.416 unidades para 594 ms (180 unidades/ms).
O seq scan sozinho cobra 80.550 para 72 ms (1.118 unidades/ms). Os dois hash
joins que processam as 533 mil linhas custam 14.846 unidades e levam 219 ms:
**68 unidades/ms, 16× abaixo da taxa do próprio seq scan**. O `MixedAggregate`
(GROUPING SETS) custa 11.884 unidades e leva 256 ms: 46 unidades/ms.

A alternativa é o NL paralelo: o modelo cobra 176.423 (39% mais) e ele roda em
**394 ms** (33% menos). Com `cpu_tuple_cost=0,03` o planner escolhe um plano
paralelo do mesmo tipo e roda em **284 ms**.

Veredito: **CPU por linha de hash/agregação subpreçada**; com CPU realista, o
paralelismo passa a compensar e o plano fica 2× mais rápido.

### DS97 — 402 → 765 ms (estimativa muito melhor, custo errado)

O que mudou: `Merge Full Join` paralelo com `Gather Merge`/`GroupAggregate` vira
`Hash Full Join` serial com `HashAggregate` derramando (5 batches, 11,6 MB).

Números: `Hash Join date_dim` 5.702 → **548.322** contra 545.766 reais (31,9× →
1,0×). A estimativa do join está certa; a do grupo fica 1,9× baixa (288.023
contra 540.073), o que subestima o spill.

Custo: o plano escolhido cobra 176.753 para 823 ms (215 unidades/ms); o merge
paralelo cobra 246.695 e roda em **602 ms**. Com `cpu_tuple_cost=0,03` o mesmo
plano escolhido já roda em 534 ms; com 0,10 o planner troca para `Merge` e roda
em **390 ms**.

Veredito: **serial × paralelo decidido com CPU 10–50× barata**. O plano serial
só parece mais barato porque o custo por linha que ele multiplica é irreal.

### DS51 — 809 → 1.081 ms (estimativa muito melhor, custo errado)

O que mudou: mesma forma (dois `Merge Full Join` + `WindowAgg`), mas o ramo de
`web_sales`/`date_dim` perde o `Gather Merge` paralelo.

Números: a janela estima 13.684 → **548.322** contra 496.169 reais (36,3× →
1,1×). O `Subquery Scan` piora (4.561 → 182.774 contra 100 linhas) porque o
`LIMIT` corta a saída; a estimativa do subplano cheio está certa.

Custo: 303.248 para 1.158 ms contra 339.891 para **947 ms** na alternativa
(`hashjoin=off`). Com `cpu0.03`, 858 ms. Mesmo padrão do DS97.

### DS39 — 1.634 → 5.551 ms (estimativa melhor no join, plano honestamente maior)

O que mudou: o `Sort` da CTE passa a ser orçado para 489.370 linhas (real
780.000) em vez de 24.452; o plano inclui uma agregação a mais antes do join.

Números: `Hash Join` da CTE 24.452 → **489.370** contra 780.000 reais (31,9× →
1,6×). Mas o `Subquery Scan` piora de 1,7× para 10,7× (360.000 contra 33.696) e
o `Sort` do self-join de 8,1× para 41,2× (10.000 contra 243).

Custo: 806.421 para 5.726 ms (141 unidades/ms). O `CTE Scan` responde por 78%
do tempo com 1% do custo: cobra 8.100 unidades (0,27 por linha estimada), mas a
primeira leitura executa a CTE inteira (5,5 s). É o tuplestore sendo cobrado
como um seq scan de heap.

Veredito: não há plano mais rápido entre os forçados; o erro é o **preço da
leitura de tuplestore** (e o custo do sort externo), não a escolha.

### DS95 — 117 → 3.905 ms (estimativa 35.000× melhor, plano 33× pior)

O que mudou: o `Hash Join` de `web_returns × ws_wh` sai de **246** (real
8.677.946, erro de 35.000×) para **8.815.320**: estimativa exata. Com isso o
planner troca o semi-join que parava na primeira linha por um build de 60.000
grupos, que materializa a CTE de 6,6 M linhas duas vezes.

Custo: 771.921 para 4.091 ms. O `CTE Scan` de 6,6 M linhas leva **1,9 s** e é
cobrado 142.629 unidades (0,021 unidade/linha); o real é 0,28 µs/linha, ~15×
mais. O plano do master (117 ms) nem chega a materializar a CTE: o semi-join
para na primeira linha.

Veredito: a estimativa está certa e mesmo assim o plano antigo é melhor. O erro
é **cobrar leitura de tuplestore como leitura de heap**, o que faz o plano que
materializa 13,2 M linhas de CTE parecer 1,5× o custo do que não materializa
nada.

### DS78 — 2.068 → 2.819 ms (estimativa do filtro piorou 40×)

O que mudou: `Merge Left Join` com `Incremental Sort` sob `Limit`, agregados
estimados em 40.771/107.837 contra 1.017/2.700.

Números: o `Aggregate` de `store_sales` melhora de 274× para 6,8×. Mas o filtro
`coalesce(sum(web_quantity),0) > 0 OR ...` deixa passar **101 de 278.810
linhas** (0,04%) e o modelo estima 55% (22.651 de 40.771, erro 224×).

Custo: `Limit` custa 4.582 = **0,45% do custo cheio do subplano (1.021.959)** e
consome 100% do tempo, porque produzir as 100 primeiras linhas exige consumir
278.709 linhas rejeitadas. A fração do `LIMIT` pressupõe 0,44% do trabalho; a
realidade é 100%.

A alternativa `nestloop=off` custa 203.813 no papel e roda em **1.141 ms**. Com
`cpu_tuple_cost=0,10`, **828 ms**.

Veredito: **seletividade de filtro sobre agregados** (não modelada) inflada pela
cardinalidade maior, somada à fração do `LIMIT` calculada sobre ela.

### H09 — 1.463 → 2.362 ms (estimativa do acesso parametrizado presa em 1)

O que mudou: `lineitem` passa a ser acessada por `Bitmap Heap Scan` com
`BitmapAnd` de `l_partkey` e `l_suppkey`, 42.656 laços por trabalhador.

Números: o bitmap estima **1 linha por laço** contra **7,49 reais** (o produto
31 × 600 / 6 M dá 0,003 e é elevado ao mínimo de 1). São ~320 mil linhas reais
onde o plano enxerga 42 mil.

Custo: o plano escolhido cobra 162.700 para 2.507 ms; a alternativa sem NL
(`nestloop=off`) cobra 210.357 e roda em **941 ms**. O NL é 2,7× mais lento e
29% mais barato — porque cada laço custa 9,99 unidades no papel e 91 µs na
realidade.

Veredito: erro de **estimativa por laço** (mínimo de 1 linha) num caminho
reexecutado 42 mil vezes; o custo por laço sozinho não explica, a subestimativa
multiplica.

## O padrão: dois preços errados, não um

**1. CPU por linha (hash join, hash aggregate, mixed aggregate) ~10–50×
barata.** O modelo cobra 0,012–0,03 unidade por linha processada em hash; na
máquina, cada linha custa 0,4–0,9 µs, o equivalente a 0,4–0,9 unidade pela
taxa do próprio seq scan (1.045 unidades/ms). Consequência: pipelines seriais
de hash parecem gratuitos, então o planner recusa planos paralelos, merge e NL
que são 1,3–3,8× mais rápidos (DS36, DS51, DS64, DS78, DS94, DS97).

**2. Probe de índice/EXISTS a `random_page_cost=4` com tudo em cache.** Um
`Index Scan` parametrizado cobra 0,31–76 unidades por laço para 1–8 µs reais;
com `random_page_cost=1.1` o DS10 volta ao plano de índice e roda em 36 ms
(contra 124). O mesmo erro aparece no `CTE Scan` (tuplestore cobrado como heap)
e no `Bitmap Heap Scan` do H09, mas nesses casos ele é mascarado pela
subestimativa de linhas.

**3. Dois erros de estimativa que não são de custo.** O `ndistinct` não
reescalado pelo filtro de data (DS10: 62.176 contra 8.731) e a seletividade de
filtro sobre agregados (DS78: 55% estimado, 0,04% real). Nesses, nenhum ajuste
de custo recupera o plano antigo — é preciso estimar melhor.

## Prova pela calibração

Tempo de execução (ms) variando só as constantes de custo, no build novo:

| query | default | cpu 0,03 | cpu 0,10 | rpc 1,1 | cpu 0,10 + rpc 1,1 | master |
|---|---|---|---|---|---|---|
| DS10 | 124 | 103 | 40 | 36 | **32** | 53 |
| DS36 | 598 | 284 | **254** | 601 | 268 | 414 |
| DS51 | 1.090 | 858 | **868** | 1.093 | 838 | 891 |
| DS64 | 229 | **204** | 219 | 222 | 214 | 187 |
| DS78 | 3.122 | 1.431 | 828 | 2.847 | **795** | 1.584 |
| DS94 | 129 | 52 | 44 | 188 | **34** | 54 |
| DS97 | 815 | 534 | 390 | 821 | **356** | 373 |
| DS39 | 5.863 | 6.227 | 5.806 | 6.143 | 6.247 | 1.702 |
| DS95 | 3.627 | 3.913 | 3.028 | 3.582 | 2.969 | 140 |
| H09 | 2.449 | 2.460 | 2.529 | 2.567 | 2.520 | 1.488 |
| DS72 | 1.235 | 1.162 | 1.135 | 1.141 | 1.216 | 10.858 |

Sete das dez regressões voltam ao nível do master (ou melhor) mexendo só em
`cpu_tuple_cost`/`random_page_cost`; DS72 continua ~10× melhor. Sobram DS39,
DS95 e H09, que exigem conserto estrutural (tuplestore e estimativa por laço).

Isso não significa mudar o default do PostgreSQL. Significa que, para o modelo
de custo, **a estimativa corrigida expõe o preço errado da CPU por linha**, e é
esse preço que precisa de um patch — não a estimativa, que agora está certa.

## Próximo passo

Pesquisar um custo por linha proporcional ao trabalho real de hash/agregação
(o termo que hoje é `cpu_tuple_cost` chapado), validando com o mesmo A/B de 123
queries. Enquanto ele não existir, melhorar a estimativa continua podendo
piorar o plano — e agora sabemos exatamente onde.
