# Limitações do PostgreSQL em estatísticas e escolha de plano

**Data:** 2026-11-24  
**Base analisada:** PostgreSQL 19.0 (lançado em 2026-11-19) e `master` (`20devel`).  
**Commit indicado nas imagens:** `0ebc9a458c` (clonado em `.claude/artifacts/2026-11-24-11-48-00_postgres-upstream-source/`).

## Convenção de evidências

- **[código]** Verificado no código do `master` nesta sessão.
- **[V]** Fonte primária lida (paper, documentação, thread ou commit).
- **[S]** Foi visto apenas o resumo de busca da fonte; a redação exata não foi conferida.
- **[I]** Inferência ou opinião.

**Textos dos papers extraídos:** `.claude/artifacts/2026-11-24-11-48-55_optimizer-paper-texts/`.

---

## 0. Resumo: onde o PostgreSQL perde mais tempo

| # | Limitação | Evidência mais forte | Suas PRs cobrem? |
|---|---|---|---|
| 1 | Cardinalidade de joins multi-tabela: independência entre cláusulas e entre tabelas, ausência de estatística cross-table e clamp em 1 linha | Com cardinalidades reais, o PG fica aproximadamente 2x mais rápido no STATS-CEB (11,34h → 5,65h) e 6,6x no JOB/IMDB-JOB (FactorJoin). No JOB, 52% das estimativas com 3 joins erram por 10x ou mais [V]. Em parte: #1/#5–#9 atacam casos específicos de um join. |
| 2 | Escolha de plano sem noção de risco: nested loop sobre input subestimado; `LIMIT` assumindo distribuição uniforme | Desligar nested loop sem índice eliminou os timeouts do JOB sem piorar nenhuma query (Leis 2015). "NL off" responde por 35% do ganho do Bao [V]. Não. |
| 3 | Nenhuma adaptatividade: não re-planeja durante a execução nem aprende entre execuções | Re-otimização em q-error 32: -45% de tempo de execução no JOB (Perron 2019). TPC-H Q9: -79%; Q21: -83% (Wu 2016) [V]. Não. |
| 4 | Estatísticas de tabela única: correlação entre colunas, `ndistinct`, staleness, partições e constantes mágicas | O estimador Duj1 subestima 30% em média e 74% com skew alto. Caso de 500M linhas: 40k estimados vs. 33M reais. Outage da Clerk em 2026 por `null_frac = 1.0` vindo da amostra [V/S]. Em parte (#9, filtro na própria chave). |
| 5 | Modelo de custo: I/O, cache, correlação física, AIO e ordem de quals | Secundário em memória e relevante em disco: erro de aproximadamente 10x mesmo com cardinalidade real (Leis 2018). Acurácia de 0,64 na escolha de access path (Heinrich 2025) [V]. Em parte (#3, #4). |
| 6 | Plan cache: heurística generic vs. custom | `num_custom_plans < 5` é arbitrário [código]. Não. |
| 7 | Enumeração de joins: GEQO e limites de collapse | GEQO fica 2,9–3,1x pior em 30–200 relações (Mancini 2022) [V]. Não. |

**Ressalva geral da literatura:** os ganhos têm cauda pesada. No Bao, a mediana melhora menos de 5%, mas o p99 cai de 130s para menos de 20s. Os baselines do PostgreSQL nos papers também variam em `work_mem`, paralelismo e GEQO (Lehmann 2024) [V].

---

## 1. Cardinalidade de joins

### 1.1 Independência e propagação de erro

**Mecanismo**

- `clauselist_selectivity_ext` multiplica as seletividades.
- Extended statistics só entram quando todas as cláusulas referenciam uma única relação:
  - `find_single_rel_for_clauses`;
  - `rel->rtekind == RTE_RELATION`. [código]
- A documentação do PG19 diz: "Extended statistics are not currently used by the planner for selectivity estimations made for table joins" [V].
- O único uso de extended stats em join é o tamanho de bucket do hash join (`6bb6a62f3c`, PG18) [V].

**Evidência**

- Leis 2015 (PG 9.4, JOB): erro de 10x ou mais em 16%, 32% e 52% das estimativas com 1, 2 e 3 joins. O erro é sistematicamente para baixo [V].
- SQLStorm (PG17): erro de duas ordens de grandeza ou mais na maioria das queries com 8 ou mais operadores. Outer joins tendem a superestimar [V].
- Han 2022, sobre onde está a folga em relação a cardinalidades reais:
  - 3,7% com 2–3 tabelas e 55,9% com 4;
  - os erros em sub-planos grandes dominam;
  - errar o operador do join raiz custou 68%; errar a ordem custou 1% [V].
- `q-error` é um proxy ruim. A correlação com runtime é 0,04, contra 0,8 do P-Error (Han 2022) [V].
- Vale medir suas PRs em tempo, como você já faz, e não em q-error [I].

**Upstream**

- Vondra, "Using extended statistics to improve join estimates" (CF 3055, 2021): só igualdade, só inner join e precisa de MCV dos dois lados. Voltou com feedback em 2022. Andy Fan reviveu em 2024 e não houve commit [V/S].
- Alexandra Wang, "Add join MCV statistics" (CF 6724), é o esforço vivo [V]:
  - Status: "Needs review" para PG20–4 (jan/2027), com Tom Lane como reviewer.
  - Sintaxe: `CREATE STATISTICS (mcv) ON t1.b, t2.e FROM t1 JOIN t2 ON (t1.a = t2.d)`.
  - Só 2-way inner equijoin, e a coluna sondada precisa de índice.
  - Objeções do Vondra: combinar amostras independentes por tabela não funciona a menos que as amostras sejam grandes; vazamento via RLS em `pg_stats_ext`; segfault em tabelas particionadas; e confusão entre `P(join | filtros)` e `P(join AND filtros)`.
  - Link: https://commitfest.postgresql.org/patch/6724/
- Tom Lane aceitaria uma versão só 2-way, desde que o catálogo comporte n-way [S].

**O que as PRs ainda não cobrem**

1. **Filtro em outra coluna da mesma tabela (até 615x).** Com um MCV estendido em (coluna_filtro, chave), dá para condicionar o MCV da chave ao filtro. Isso continua diretamente ligado à #9. É o que o CF 3055 fazia ("use restrictions as conditions to filter the MCV lists") [S]. Levar em conta o alerta do Vondra sobre `P(join | filtros)` vs. `P(join AND filtros)`.
2. **Correlação entre tabelas em star joins (4,2M x).** Exige estatística de join (CF 6724) ou amostragem no planejamento.
3. **Semi, anti e outer joins.** As PRs só tratam inner joins. `eqjoinsel_semi` tem a mesma cegueira para filtros e chaves compostas [I].

### 1.2 Clamp em 1 linha e inconsistência da estimativa

**Mecanismo do clamp**

- `clamp_row_est()` força no mínimo 1 linha [código].
- Como as estimativas multiplicam, o "1" vira uma cauda absorvente:
  - Kepler: 64% das estimativas de join no Stack são exatamente 1 [V].
  - LpBound (PG 13.14): "the estimate is 1 for all SM queries", 7–8 ordens de grandeza abaixo do real [V].
  - Neo: o PG "always estimates the cardinalities for the final joined result to be close to 1" [V].

**Inconsistência entre ordens de enumeração**

- O `rows` de um `joinrel` é calculado uma vez, pelo primeiro par (`outer`, `inner`) que constrói o `joinrel` (`build_join_rel` → `set_joinrel_size_estimates`; `relnode.c:991`) [código].
- Leis 2015, nota 6: só permutando a ordem de `FROM/WHERE`, a estimativa variou entre 3, 9, 128 e 310 linhas, com valor real de 2600. A causa é o arredondamento para 1 somado a domínios inconsistentes [V].
- Lepikhov 2024: o `ndistinct` usado para uma equivalence class depende de qual membro aparece primeiro, ou seja, muda com o texto do SQL [S].
- A proposta de Evdokimov (2025) de remover o arredondamento recebeu pushback, porque estimativas abaixo de 1 deixam o planner otimista demais com nested loops [S].

**Oportunidade [I]**

Tornar o tamanho do `joinrel` independente do par e da ordem: um par canônico, ou `ndistinct` canônico por EC, como o DuckDB faz com o "total domain" por equivalence set [V]. É mensurável do mesmo jeito que suas PRs: planos byte-idênticos quando não há mudança.

### 1.3 Inequality joins e "Var op Var"

- As PRs #1/#5 preenchem uma lacuna que o próprio upstream reconheceu.
- No patch de seletividade de join para range types (CF 3821, Schoemans/Repas/Sakr, 2022–23, revivido por Haibo Yan em abr/2026), Tom e Tomas acharam "a little odd that range join selectivity would become smarter than scalar inequality join selectivity". Os autores planejavam "a second patch that applies these ideas to scalar inequality" [S].
- Postar a #5 nessa thread, ou referenciá-la, é o melhor argumento de aceitação disponível [I].

**Lacuna restante: `Var op Var` na mesma tabela**, por exemplo `t.start_at < t.end_at`.

- `scalarineqsel` exige `Const` do outro lado e cai em `DEFAULT_INEQ_SEL` [código].
- Extended stats também não ajudam: `statext_is_compatible_clause_internal` diz que "in the future, the range of supported clauses may be expanded to more complex cases, for example (Var op Var)" [código].
- O caso típico é fortemente correlacionado (`start < end` vale quase sempre). Independência entre histogramas melhora em relação a 1/3, mas não resolve.
- O MCV estendido sobre (a, b) é o lugar certo para isso [I].

### 1.4 Limites superiores (pessimistic estimation)

| Trabalho | Resultado no PostgreSQL |
|---|---|
| Cai et al. 2019 (PG 9.6) | -43% com índices de FK; 24.725s → 2.304s sem PK. Construir os sketches de forma ingênua custou mais que a execução [V]. |
| SafeBound 2021 (PG 13) | Runtime 20–85% menor. Extended stats do PG "do not significantly alter the estimates" [V]. |
| LpBound 2025 | 1,5–10,5 ms de estimativa por query [V]. |
| UES, CIDR 2021 (PG 12.4) | JOB de 464s para 259s, só com PK [V]. |
| PARQO 2024 (PG 16.2, seleção robusta) | 3,23x no JOB, 2,01x no DSB, 1,36x no STATS [V]. |

- Não há hook para tamanho de join. Han et al. tiveram que sobrescrever `calc_joinrel_size_estimate`.
- O `clauselist_selectivity_hook` de Lepikhov (2025) levou -1 [S].
- O PG19 trouxe `planner_setup_hook`, `joinrel_setup_hook`, `join_path_setup_hook` e `build_simple_rel_hook` [V].
- Se `joinrel_setup_hook` permite sobrescrever `rows`, não foi verificado [I].

---

## 2. Escolha de plano sem noção de risco

### 2.1 Nested loop sobre input subestimado

- O mecanismo é `add_path`, comparando custos pontuais com `STD_FUZZ_FACTOR 1.01` [código]. Nenhum path carrega incerteza.
- O que a literatura mostra:

| Experimento | Resultado |
|---|---|
| Leis 2015: desligar NL sem índice | Zero timeouts, nenhuma query mais lenta [V]. |
| Bao: hint "NL off" | 35% do ganho [V]. |
| AutoSteer | "NL off" é o hint número 1 [V]. |
| Leis 2018, em disco | O PG escolhe 788 INL contra 669 no ótimo [V]. |
| Gao: NL off global | Pior no geral; aproximadamente 50x na query 24b [V]. |
| Balsa: NL off global | 60% mais lento [V]. |

O problema é decidir **quando** aplicar o NL, não banir o NL.

**Upstream**

- Haas, "Disfavoring unparameterized nested loops" (2021, retomado em 2022). Geoghegan concordou. Tom discordou: o plano atual é "optimal, and easily proven to be so, in some very common cases". Nada foi commitado [S].
- Thread: https://www.postgresql.org/message-id/CA%2BTgmoYtWXNnD69ZXuUjT_YFm12dWa1XXM9EY-CRcrZqmbagmail.gmail.com

**Oportunidade [I] — maior impacto e mais polêmica**

Guardar no `RelOptInfo` a proveniência ou confiança da estimativa:

- **Provada:** chave única, FK (`get_foreign_key_join_selectivity`), `= const` em coluna única ou bound da sua #8.
- **Derivada de produto de independência ou de defaults:** todo o resto.
- Penalizar NL não parametrizado só no segundo caso responde à objeção do Tom, porque os "very common cases" dele são justamente os provados.

### 2.2 `ORDER BY ... LIMIT` (row goal)

**Mecanismo**

O custo do `LIMIT` interpola linearmente entre startup e total, assumindo que as linhas que passam no filtro estão uniformemente espalhadas na ordem do índice [V].

**Evidência**

- pganalyze: custo 35 vs. aproximadamente 43.000 e 2M de "Rows Removed by Filter" [V].
- GoCardless: tabela de 500M linhas e 17,5M lidas para um `LIMIT 2` [V].

**Upstream**

- Em 2022, Tom disse que não há estatísticas adequadas para detectar isso [V].
- Em nov/2025, uma GUC para desligar o ajuste do `LIMIT` foi rejeitada como "an extremely blunt instrument".
- **Direção sugerida pelo Tom:** "inflate the LIMIT's minimum cost by some factor based on the likelihood of that sort of error" [S].
- Thread: https://www.mail-archive.com/pgsql-hackers@lists.postgresql.org/msg218884.html

**Oportunidade [I]**

Implementar exatamente essa direção. Combina com a confiança da 2.1: o fator cresce quando a seletividade do filtro é baixa e não provada, e quando as colunas do filtro não são o prefixo do índice.

### 2.3 Memoize e ordem das chaves

**Rowley, 2025**

- A estimativa de 1.250 contra 83.334 reais fez o planner achar que tudo cabia no cache.
- Como a ordem de lookup coincidiu com a ordem de eviction, houve zero hits: 98,9s com Memoize contra 55,8s sem.
- Ideias levantadas: estatística de ordem de valores ou detecção em runtime [V].
- O PG19 mostra as estimativas do Memoize no `EXPLAIN` [V].

---

## 3. Falta de adaptatividade e de feedback

- No core não existe re-planejamento durante a execução, nem feedback entre execuções [V], ausente das release notes.

**Evidência canônica**

- Perron 2019 (PG 10): -45% de tempo de execução no JOB. Corrigir só joins de 1–3 tabelas deu "virtually no benefit" [V].
- Wu 2016: em queries de "tortura" correlacionadas, todos os planos re-otimizados rodaram em menos de 1s, contra centenas a milhares de segundos [V].
- Zhang et al. 2023 (bloom semijoins + troca hash/NL): 1,5–2,6x, igualando ou superando otimizadores aprendidos [V].

**Outros SGBDs**

- Oracle: adaptive join methods e statistics feedback.
- SQL Server:
  - Adaptive join (batch mode).
  - CE feedback (2022), que testa uma suposição alternativa e só persiste se verificar.
  - Interleaved execution para MSTVF.
  - PSP (até 3 predicados × 3 buckets) [V/S].

**Extensões para PostgreSQL**

- AQO precisa de patch no core.
- O "replan" do Postgres Pro é proprietário [S].
- `pg_plan_advice` + `pg_stash_advice` (contrib no PG19) só restringem escolhas: "you will only ever get a plan that the core planner would have considered". Não cobrem estimativas, agregação nem set operations [V].

**Oportunidade de médio porte [I]**

"Interleaved execution" para fontes opacas, como CTE `MATERIALIZED`, temp table recém-populada e SRF: executa a subárvore, mede e planeja o restante.

---

## 4. Estatísticas de tabela única

### 4.1 Correlação entre colunas (`CREATE STATISTICS`)

**Limites atuais**

- Criação é manual.
- `dependencies` só para `= const` e `IN` (`F_EQ_SEL` em `dependencies.c`) [código].
- Não há "Var op Var" [código], nem uso em join [V].
- Dependencies assumem cláusulas compatíveis. O exemplo de ZIP/cidade na documentação dá 1 linha na realidade [V].

**Outros SGBDs**

- Oracle tem `SEED_COL_USAGE` e `AUTO_STAT_EXTENSIONS`.
- SQL Server tem `AUTO_CREATE_STATISTICS` [V/S].

**Extensão**

- `pg_index_stats` (Lepikhov 2025) cria extended stats a partir de índices multicoluna [S].

### 4.2 `ndistinct`

**Mecanismo**

- Amostra de `300 * attstattarget` linhas (30k no default) [código].
- Estimador Haas–Stokes Duj1 [código].
- Amostragem em dois estágios (blocos, depois linhas); valores agrupados fisicamente parecem duplicados.

**Evidência**

- Duj1 a 5% de amostra: viés médio de aproximadamente -30,5% e -74% com skew alto [V].
- Charikar 2000 dá um piso de erro de aproximadamente 10,7x para 1e7 linhas com 30k de amostra (aritmética do agente, com gamma em vez de log) [V].
- GoCardless: 11.029 distintos com `SYSTEM` contra 25.351 com `BERNOULLI` [V].
- 500M linhas: 40k estimados contra 33M reais. Target 5000 ainda deixa aproximadamente 100x de erro [S].
- Candidata a integrar: o `ndistinct` verdadeiro representa o `O(n)`; "two wrongs make a right" (Leis 2015) [V]. Corrigir uma estimativa isolada pode expor outra.

**Upstream**

- Nenhum estimador novo no 17, 18 ou 19. A RFC de HLL + MCV do van Meent (2022) parou [S].
- No master (PG20), `906b61a019` (Richard Guo, jul/2026) propaga `stadistinct` por subqueries/CTE com `GROUP BY/DISTINCT`. Remove o fallback 1/200, descrito como "a significant factor in poor TPC-H benchmark performance" [V].

**Oportunidade [I]**

`ndistinct` via HLL com scan completo, **opt-in por coluna** (opção de atributo ou de `ANALYZE`). Não muda o plano de quem não usa.

### 4.3 Staleness e partições

**Autovacuum**

- Dispara com `50 + 0.1 * reltuples`, sem teto [código]. São 10M linhas modificadas numa tabela de 100M.
- O PG18 criou `autovacuum_vacuum_max_threshold`, mas só para vacuum [código].
- O SQL Server usa `MIN(500 + 0.2n, SQRT(1000n))`, aproximadamente 316k para 100M linhas [V].

**Partições**

- Pais particionados nunca recebem autoanalyze [V do PG19].
- O commit `0827e8af` (2021) foi revertido antes do PG14 por ineficiência com muitas partições [S].
- Laurenz Albe (2025) mostra 500.000 estimados contra 100 reais até um `ANALYZE` manual, e propõe somar os contadores das partições [V].

**Ascending keys**

`get_actual_variable_range` só sonda o índice quando:

- a coluna tem índice btree;
- a comparação é de range e cai no bucket extremo;
- a tabela não é particionada (`RELKIND_PARTITIONED_TABLE`, `selfuncs.c:7052`);
- a sondagem fica em no máximo `VISITED_PAGES_LIMIT 100` páginas [código].

### 4.4 A amostra "provando" ausência

**Outage da Clerk, 2026-02-19 [V]**

- A coluna era 99,9996% `NULL`, e a amostra só viu NULLs.
- O planner estimou zero não-nulos contra mais de 17k reais.
- Mais de 95% do tráfego tomou 429 por 90 minutos.

**Oportunidade pequena [I]**

Quando a seletividade sai 0 (ou `null_frac = 1.0`) a partir de uma amostra muito menor que a tabela, usar um piso estatístico. Pela "regra de 3", o limite superior de 95% é aproximadamente `3/n_amostra`; um ponto médio como `1/(2*n_amostra)` daria aproximadamente 1,7k linhas em 100M. Só afeta o caso degenerado. É o tipo de mudança que as PRs fazem bem: no-op fora do caso, com teste por propriedade.

### 4.5 Constantes mágicas e fontes opacas

| Onde | Valor |
|---|---|
| `DEFAULT_EQ_SEL` | `0.005` |
| `DEFAULT_INEQ_SEL` | `1/3` |
| `DEFAULT_MATCHING_SEL` | `0.010` |
| `DEFAULT_NUM_DISTINCT` | `200` |
| LIKE sem prefixo | `FIXED_CHAR_SEL 0.20`, `ANY_CHAR_SEL 0.9`, `FULL_WILDCARD_SEL 5.0`. Usa o histograma só se ele tiver 100 entradas ou mais [V]. |
| SRF | `prorows`, default 1000 |
| FDW | `rel->rows = 1000`; comentário: "entirely bogus default estimate" [código] |
| Tabela nunca analisada | Mínimo de 10 páginas (`tableam.c:725`) |
| Valores > 1 KB | `WIDTH_THRESHOLD 1024`: ficam fora do MCV e histograma |
| `jsonb` | Sem estatística por path. O patch Glukhov/Vondra parou, entre outras coisas por risco de OOM no `ANALYZE` [S]. |

- LIKE no JOB (PG 14.5): q-error p90 de 91 e p99 de 451 (LPLM 2024) [V].
- Já corrigido:
  - PG18: `generate_series` e `IN (VALUES)` → `= ANY` [V].
  - PG19: funções booleanas usam estatísticas de expressão (`1eccb93150`) [V].

---

## 5. Modelo de custo

**Peso relativo**

- Em memória, o efeito do modelo de custo é "dwarfed" pelo da cardinalidade. Ajustar o modelo deu 41%; um modelo trivial de contagem de tuplas deu 34% (Leis 2015) [V].
- Em disco é outra história (Leis 2018) [V]:
  - o erro é de aproximadamente 10x mesmo com cardinalidade real;
  - com `random_page_cost` de 4 para 100, 63% das queries ficaram mais rápidas e 35% mais lentas.

**Escolha de access path**

A acurácia é de aproximadamente 0,64 (aleatório seria 0,5), e o PG troca para seq scan "far too early" mesmo com q-error de scan de no máximo 1,05. É erro de custo, não de cardinalidade (Heinrich 2025) [V].

### 5.1 I/O e AIO

- Vondra (out/2025) mede `random_page_cost` de aproximadamente 20 ou mais em SSD, sem cache [S].
- Andres Freund [V]:
  - Index scan para frente e para trás custa igual, mas mede 36ms vs. 2123ms por causa do readahead.
  - `effective_io_concurrency` é ignorado no custo de bitmap heap scan.
  - Ele cita o Tom: AIO "is going to require some radical changes in the planner's I/O cost models".
- O resultado foi só um rewrite da documentação (`3896e861b3`).
- **Correlação física:** um número por coluna, e o btree usa só a da coluna líder (`if (indexcol == 0) ... btcost_correlation`) [código]. Entra em `cost_index` como `csquared`, interpolando entre o I/O mínimo e o máximo [código]. Propostas de correlação por índice (Przybył 2017/2019) não entraram [S].
- **Ordem de quals:** `order_qual_clauses` ordena só por `security_level` e depois custo [código]. O próprio comentário diz que considerar seletividade "would likely do the wrong thing" porque a lista pode conter entradas removidas depois. A ordenação por rank, `(sel - 1) / custo`, só faria diferença com funções caras.
- **Largura:** `cost_seqscan` cobra `cpu_tuple_cost + qual` por tupla, sem termo de largura nem de detoast. Deforming de tuplas largas e detoast não são cobrados [I].
- **AIO:** desligado por default no PG19 porque a ativação por custo "has been determined to be unreliable" (`boot_val => false`) [código + V].

**Atenção para a PR #3 [V]**

Houve vários commits recentes em `final_cost_hashjoin` / `estimate_hash_bucket_stats`:

- Vondra: `a1b4f289be` (PG18) e `b85c4700fc` (out/2025).
- Tom: `bd3e3e9e56` (dez/2025), `d80b022501` e `e6a1d8f5ac` (mar/2026, reescreveu a correção de skew).

Esperar conflito no rebase.

### 5.2 Ordem de quals

`order_qual_clauses` ordena só por `security_level` e depois custo [código]. O próprio comentário diz que considerar seletividade "would likely do the wrong thing", porque a lista pode conter entradas removidas depois. A ordenação por rank `(sel - 1) / custo` só faria diferença com funções caras.

---

## 6. Plan cache (prepared statements)

**Mecanismo**

- 5 custom plans ("arbitrary").
- Depois disso, usa um generic se `generic_cost < avg_custom_cost` [código].
- O generic plan estima `col = $1` por `ndistinct`.
- O custo do generic plan ignora o pruning de partições em runtime (Rowley 2019, Vondra 2024) [S].

**Upstream**

- O "Don't lock partitions pruned by initial pruning" do Langote foi revertido em mai/2025 [V/S].
- Propostas de decisão sensível a skew (detecção via MCV) não entraram [S].
- O PG19 só adicionou contadores de generic/custom no `pg_stat_statements` [V].

---

## 7. Enumeração de joins

- Defaults: `join_collapse_limit = 8`, `from_collapse_limit = 8`, `geqo_threshold = 12` [código].

**Limiares de Neumann & Radke 2018**

- DP exato abaixo de 14 relações, ou com até 10k subgrafos conexos.
- DP linearizado até 100 relações; greedy acima disso.
- O limiar olha a forma do grafo, não a contagem de relações [V].

| Método | Tempo de otimização | Custo do plano |
|---|---:|---:|
| Adaptativo | 53 ms | 1,00 |
| DPhyP | 213 ms | 1,00 |
| Genético (estilo PG) | 6,6 s | 1,02 |

Com 100 relações, o genético fica 5,6x pior em média [V].

- GEQO: 1,9–3,1x pior em 30–200 relações (Mancini 2022, PG 12) [V].
- Só desligar GEQO muda a query 24b do JOB em 9,9x (Lehmann 2024) [V].
- Upstream: o GO greedy de Chengpeng Yan, com review do Vondra, está com CF em "Withdrawn", mas a thread segue ativa até nov/2026 (v7). O Vondra viu aproximadamente 80% dos planos do TPC-DS mudarem [V/S].

---

## 8. Suas PRs: prior art upstream e riscos

| PR | Prior art / concorrência | Nota |
|---|---|---|
| #1/#5 inequality join | CF 3821 (range types, revivido em abr/2026). Tom/Tomas pediram o equivalente escalar [S]. | Melhor gancho para submeter. |
| #6/#7 self-join (`ndistinct`/extstats) | `6bb6a62f3c` (PG18, extstats no bucket do hash). Patch de He Kuang (nov/2026, v3) para estimar bucket de hash join por `ndistinct` conjunto das chaves [S]. | Coordenar com He Kuang. |
| #8 chave única composta | FK join selectivity (9.6) é o análogo para FK [código]. Não achei concorrente direto [I]. | Nenhuma. |
| #9 distribuição condicional | CF 3055 (Vondra 2021) já filtrava MCV por restrição [S]. `057012b205` (PG19) refatorou o MCV matching em `eqjoinsel` para hash [V]. | Rebase sobre `057012b205`. |
| #4 custo de sort | `db0d67db2` revertido (`f4c7410e`). Lepikhov 2024 (Rowley: "fooled when the first column is distinct") [S]. | Sua descrição já responde ao histórico. |
| #3 hash spill | Commits do Tom e do Vondra em 2025–26 (seção 5) [V]. | Risco de conflito. |
| #10 loose index scan | CF 1741 (Dolgov/Pedersen/Munro, devolvido). O skip scan do PG18 é outra coisa: pula colunas líderes omitidas [V]. | Nenhuma. |

---

## 9. Candidatos para próximas PRs

Ordenados por impacto segundo a evidência, aderência ao estilo (usar estatística existente, no-op sem ela, medido) e risco de aceitação. O ranking é opinião [I].

| # | Ideia | Impacto | Tamanho | Risco de aceitação |
|---:|---|---|---|---|
| 1 | Confiança/proveniência da estimativa no `RelOptInfo`, usada para penalizar NL não parametrizado e para inflar o custo mínimo do `LIMIT` (direção do Tom, nov/2025) | Alto: ataca os itens 1 e 2 da literatura | Grande | Alto, mas é mais estreita que a proposta do Haas |
| 2 | `autovacuum_analyze_max_threshold` (espelha o PG18) + autoanalyze de pai particionado somando contadores das filhas | Alto na prática | Pequeno/médio | Baixo para o teto; médio para partições, que já tiveram um revert |
| 3 | Filtro em outra coluna via MCV estendido (continuação da #9) | Médio–alto (até 615x) | Médio | Médio; coordenar com o CF 6724 |
| 4 | Piso estatístico quando a amostra "prova" zero (`null_frac = 1.0`, seletividade 0) | Médio (raro, mas catastrófico) | Pequeno | Baixo–médio |
| 5 | Tamanho de `joinrel` e `ndistinct` de EC independentes da ordem de enumeração | Médio | Médio | Médio |
| 6 | `Var op Var` na mesma tabela: reaproveitar a #5 + suporte no MCV estendido | Médio | Pequeno/médio | Baixo–médio |
| 7 | Estender #5/#8/#9 para semi e anti joins | Médio | Médio | Médio |
| 8 | Plan cache sensível a skew (MCV do parâmetro) | Médio | Médio | Médio |
| 9 | `ndistinct` HLL opt-in | Médio | Médio | Médio |
| 10 | Ordem de quals por rank quando os custos diferem muito | Baixo–médio | Pequeno | Baixo |

**De longo prazo**

- Interleaved execution para CTE materializada, temp table e SRF.
- Modelo de I/O com readahead e AIO.
- Review do CF 6724 (join MCV), que fica no centro da sua área e tem o Tom como reviewer.

---

## Fontes principais

### Papers e documentos

- Leis 2015: https://www.vldb.org/pvldb/vol9/p204-leis.pdf
- Leis 2018: https://db.in.tum.de/~leis/papers/lookinaclass.pdf
- CommitFest 6724: https://commitfest.postgresql.org/patch/6724/
- Thread sobre custo de `LIMIT`: https://www.mail-archive.com/pgsql-hackers@lists.postgresql.org/msg218884.html

> **Nota de transcrição:** o conteúdo foi reconstruído a partir das fotografias do documento na tela. Pequenos trechos de URLs, hashes, nomes próprios e números nas imagens têm baixa legibilidade; quando não foi possível confirmar com segurança, a redação foi mantida no nível de detalhe legível ou marcada como aproximada.

---

## 10. Investigação: subqueries correlacionadas e estimativas otimistas

### 10.1 Hipótese observada nas regressões

A hipótese registrada nas fotografias é que uma regressão pode surgir quando uma PR reduz a estimativa de cardinalidade em um nível interno: a subquery passa a parecer quase gratuita e o planner aceita uma ordem que, na execução real, processa muito mais linhas.

- Candidatas mencionadas: PR #5 ("disjoint ranges", cuja estimativa pode cair para 1 linha) e PR #9 (estimativa de 668.000 para 2.999).
- Isso pode desfazer compensações acidentais entre erros ("two wrongs make a right", de Leis): um nó antes superestimado empurrava o plano para hash; quando essa estimativa é corrigida, uma subestimativa acima dele fica exposta.
- A distinção proposta é importante: o primeiro caso pode ser um problema de modelo, não de risco; os outros casos se relacionam ao item 1 (risco decorrente de estimativas incertas).
- A observação sobre *predicate migration* foi explicitamente marcada como inferência, não conferida linha a linha.

### 10.2 Por que uma subquery correlacionada pode parecer barata

A análise do `master` apontou três mecanismos que podem se combinar:

1. **A subquery é planejada antes de se conhecer o número de chamadas.** `make_subplan` escolhe o caminho mais barato para uma execução usando `get_cheapest_fractional_path(final_rel, tuple_fraction)`. Isso ocorre antes de existirem os `RelOptInfo` da query externa.
2. **O custo de startup é pago a cada chamada correlacionada.** Com `parParam != NIL`, o custo de startup é somado ao custo por tupla (`per_tuple`).
3. **A alternativa hash tem cobertura limitada.** Existe `AlternativeSubPlan`, mas somente para `EXISTS` simples. Durante a busca de joins, a alternativa é custeada usando o primeiro plano ("Arbitrarily use the first alternative plan for costing"). A escolha que considera o número de execuções só ocorre depois, em `setrefs.c`.

O número de chamadas é estimado pelas linhas no nível em que o qualificador fica. O qualificador é colocado no nível mais baixo que contém suas `Vars`; não há posicionamento por custo (o que a anotação associa a *predicate migration*).

### 10.3 Diagnóstico antes de escrever código

**Queries TPC-DS para conferir no kit:**

- Subqueries correlacionadas escalares/agregadas: Q1, Q6, Q30, Q32, Q41, Q81 e Q92.
- Casos com `EXISTS` que viram semi/anti join: Q10, Q16, Q35, Q69, Q94 e Q95.

A lista foi anotada de memória e deve ser conferida no kit local.

**Comparação com `pg_plan_advice`:**

1. Gerar `EXPLAIN (PLAN_ADVICE)` no build `master` e aplicar a advice no build com as PRs.
2. Observar quanto o planner novo estima que o plano bom custa.
3. Se ele considera o plano bom mais caro, mas o plano bom roda muito mais rápido, o erro está nas estimativas ou no modelo de custo.
4. Conferir o feedback `matched/failed` para garantir que a advice foi aplicada integralmente.
5. No plano ruim, procurar o nó mais baixo em que `estimated rows × loops` e `actual rows × loops` divergem por 10x ou mais.

**Classificação sugerida do nó culpado:**

- `outer` do nested loop estimado perto de 1 linha: item 1 (cardinalidade).
- Custo por chamada da subquery errado: mecanismo 1 da seção 10.2 (planejamento sem conhecer o número de chamadas).
- `loops` muito acima do estimado: mecanismo 2 da seção 10.2 (custo de execução repetida).

### 10.4 Proposta: limite superior de cardinalidade e custo do arrependimento

A proposta é carregar, junto da estimativa pontual, um limite superior derivado das estatísticas existentes e cobrar o arrependimento esperado. É a linha de SafeBound/LpBound, mas usando somente `pg_statistic`.

#### Passo 1 — `rows_hi` em `RelOptInfo` e `ParamPathInfo`

Para uma cláusula de tabela única:

- `const` presente no MCV: usar a frequência, com margem de amostragem.
- `const` fora do MCV: usar, no máximo, o limiar de inclusão no MCV. É um limite estatístico, não uma prova.
- Faixa (*range*): somar os buckets tocados, contando integralmente os buckets parcialmente tocados.
- `DEFAULT_*`, `Var op Var` e funções opacas: `sel_hi = 1`.
- `const` em coluna única: uma linha; caso considerado provado.

Para conjunções, um limite válido é o mínimo dos `sel_hi`, não o produto. O produto só vale sob independência. A distância entre esse mínimo e a estimativa pontual mede o risco da hipótese de independência.

Para joins, a fórmula anotada é:

```text
join_rows_hi = min(
    outer_hi * inner_hi * sel_hi,
    outer_hi * maxdeg_inner,
    inner_hi * maxdeg_outer
)
```

`maxdeg` é a maior frequência da chave (o topo do MCV) multiplicada pelo número de tuplas da tabela-base. Filtros só reduzem esse valor, portanto o limite continua válido. Chave única dá `maxdeg = 1`, algo que a PR #8 já calcula; FK também fornece um limite. Para semi e anti joins, o limite é no máximo `outer_hi`.

#### Passo 2 — aplicar o limite somente onde o custo cresce de forma superlinear

- Nested loop não parametrizado: `regret = (outer_hi - outer) * custo_do_rescan_do_inner`.
- SubPlan correlacionado: `regret = (calls_hi - calls) * per_call_cost`.
- `LIMIT`: no pior caso, a fração vai para 1. Essa é a direção sugerida por Tom em nov/2025.
- Hash e merge crescem linearmente; portanto, o regret tende a ser pequeno e quase não muda o plano.

Custo ajustado:

```text
cost' = cost + λ * regret
```

`λ` seria uma GUC com default 0. Assim, nada muda até a GUC ser ativada. O regret entra no próprio `total_cost`, sem acrescentar uma nova dimensão de comparação ao `add_path`.

A justificativa de aceitação em relação à proposta de Haas é preservar os casos que Tom chamou de "optimal and easily proven": PK, chave única e FK. Nesses casos, `rows_hi = rows`, o regret é zero e o plano não muda. A penalidade aparece somente quando o limite é frouxo.

### 10.5 Validação em fases

1. Instrumentar `rows_hi` com `λ = 0` e exibir o valor no `EXPLAIN`. A primeira métrica é a taxa de violação (`actual > rows_hi`) no JOB e no STATS-CEB. Ela deve ficar próxima de zero.
2. Ativar o regret em nested loop e SubPlan e variar `λ` em JOB, STATS-CEB, TPC-DS e TPC-H. O JOB é o benchmark que mais sofre com nested loops.
3. Fazer o ajuste de `LIMIT` por último.
4. Medir o tempo de planejamento, como na PR #1. Uma possibilidade é guardar `norm_selec_hi` em `RestrictInfo`, ao lado de `norm_selec`.

### 10.6 Alternativa para o mecanismo de planejamento da subquery

Se o diagnóstico apontar o mecanismo 1 da seção 10.2, a correção mais direta seria estender `AlternativeSubPlan` para subqueries escalares agregadas com correlação por igualdade. A alternativa calcularia um `GROUP BY` uma vez, usando hash; `setrefs` escolheria entre as alternativas conforme o número de execuções.

A hipótese é que isso atacaria Q1, Q6, Q30, Q32, Q81 e Q92. A própria anotação ressalva que isso ainda não foi medido: é uma hipótese.

### 10.7 Custo por chamada condicionado à distribuição do parâmetro

A investigação complementar separa três grandezas: número de chamadas, custo médio por chamada e quantidade de valores distintos do parâmetro. Esta seção registra a segunda; a nota `correlated-subplan-cost-estimation.md` contém a análise completa.

Para uma igualdade interna como `cs.item_sk = $1`, o caminho `var_eq_non_const` usa uma estimativa genérica próxima de `(1 - null_frac) / ndistinct`, tratando o valor desconhecido como igualmente provável. Isso ignora a distribuição da coluna externa que fornece `$1`.

Definindo (P_{outer}(v)) como a probabilidade de a entrada externa fornecer (v), e (f_{inner}(v)) como o número de linhas internas que correspondem a (v), a cardinalidade esperada por chamada é:

$$
E[inner\_rows\ per\ call] =
\sum_v P_{outer}(v) \cdot f_{inner}(v)
=
\frac{|outer \bowtie_{key} inner|}{|outer|}
$$

Para custo por chamada, a forma geral é:

$$
E[inner\_cost] =
\sum_v P_{outer}(v) \cdot inner\_cost(v)
$$

Se as distribuições externa e interna forem iguais e (f(v)) for a frequência relativa do valor, a fração esperada de correspondências é proporcional a:

$$
\sum_v f(v)^2
$$

Isso torna explícito o efeito do *skew*: valores frequentes são mais prováveis na entrada externa e também tendem a produzir mais linhas na subquery. Com distribuições diferentes, deve-se manter a ponderação geral, sem assumir que ambas são iguais.

A cardinalidade média por chamada pode ser relacionada diretamente à estimativa de join por igualdade dividida pelas linhas externas; para correlação por desigualdade, a hipótese anotada é usar a estimativa correspondente à PR #5. A distribuição marginal da coluna-base ainda não incorpora automaticamente filtros e joins externos (por exemplo, filtros em `manufacturer_id` podem alterar a distribuição de `item_sk`).

### 10.8 Próximo passo registrado

Preparar um script de triagem que rode as queries nos dois builds, extraia `estimated rows` e `actual rows × loops` de `EXPLAIN (ANALYZE, FORMAT JSON)` e aponte o nó culpado. O script seria executado na máquina que contém o TPC-DS.

Uma sessão anterior do Claude nessa máquina foi mencionada como possível fonte da query que regrediu, mas o acesso ao transcript foi negado. A anotação registra que seria necessária autorização explícita para ler a sessão `3b2e548f`.
