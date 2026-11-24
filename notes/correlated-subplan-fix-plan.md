# Plano de correção: subqueries correlacionadas

**Data:** 2026-11-24  
**Base indicada:** `master` (`20devel`), commit `0ebc9a458c`, no clone usado na investigação.  
**Origem:** transcrição consolidada das capturas de tela da investigação.  
**Status:** plano técnico; as referências a funções/linhas e as hipóteses de integração ainda precisam ser revalidadas no commit exato antes de qualquer patch.

## 1. Resumo executivo

A investigação separa três problemas que não devem ser tratados como uma única estimativa:

1. **Número de chamadas:** quantas vezes o SubPlan ou qualificador correlacionado será executado.
2. **Custo por chamada condicionado ao parâmetro:** qual é o trabalho esperado dentro da subquery, ponderado pela distribuição dos valores fornecidos pelo lado externo.
3. **Cache por valor do parâmetro:** quantos valores distintos chegam e se execuções repetidas podem reutilizar resultados.

| Item | Situação observada | Proposta | Ganho esperado | Risco |
|---|---|---|---|---|
| 3. Cache | O executor já tem infraestrutura de `Memoize` no topo de planos parametrizados; falta gerar/custear uma variante para SubPlan | Criar variante memoizada e escolher entre ela e o plano normal em `setrefs` | Alto em workloads com poucos valores distintos, especialmente TPC-DS | Médio/alto: semântica e integração com SubPlan |
| 2. Custo por chamada | `var_eq_non_const` usa uma hipótese uniforme para o parâmetro | Usar a distribuição da coluna externa, primeiro no caso simples de tabela-base | Médio; pequeno em TPC-DS para a fase inicial | Médio |
| 1. Chamadas | `setrefs` usa uma aproximação baseada em `plan_rows * 2`; startup/rescan pode ser cobrado incorretamente | Separar número de avaliações, custo de rescan e short-circuit | Pequeno/médio, e parcialmente mitigado pelo item 3 | Baixo/médio |

**Ordem de trabalho recomendada:** diagnóstico da regressão; infraestrutura comum para resolver `PARAM_EXEC`; item 3; item 2a; pesquisa do item 2b; item 1. A infraestrutura comum atende tanto ao item 2 (estatísticas da coluna externa) quanto ao item 3 (ndistinct da chave de cache).

## 2. Diagnóstico no código e comportamento atual

### 2.1 Planejamento e parâmetros externos

A trilha anotada nas capturas é:

1. O parser permite uma Var externa (`varlevelsup > 0`).
2. `subquery_planner` / `pull_up_sublinks` transformam alguns casos em semi/anti join quando possível.
3. No pré-processamento, `SS_process_sublinks` / `SS_replace_correlation_vars` substituem Vars externas por `Param` (`PARAM_EXEC`).
4. `assign_param_for_var` registra a Var no `plan_params` do nível dono, com `varlevelsup = 0`.
5. Para subqueries correlacionadas, o custo de startup pode ser somado ao custo por chamada; a anotação aponta `cost_subplan` em `costsize.c` como ponto a conferir.
6. Para alguns casos simples de `EXISTS`, há `AlternativeSubPlan`: plano normal e alternativa hash. A escolha final considera o número de execuções em `setrefs.c`, mas o custo durante a busca de joins usa arbitrariamente a primeira alternativa.

A captura também registra o comentário do código de que o planner está “too early” para estimar a query externa. Isso é central: a subquery é planejada antes de o nível externo ter seus `RelOptInfo` e sua distribuição efetiva completamente disponíveis.

### 2.2 Número de chamadas e execução repetida

- O qualificador é avaliado no nível mais baixo que contém suas Vars. O número de avaliações depende, portanto, da cardinalidade estimada desse nó.
- A captura aponta `num_exec` em `setrefs.c` como uma estimativa derivada de `plan_rows * 2.0` para qualificador; isso deve ser confirmado no commit-alvo e distinguido da contagem de linhas de entrada de um scan.
- `ExecScanSubPlan` detecta `parParam` e chama `ExecReScan` quando os parâmetros mudam.
- Nós que não dependem do parâmetro não precisam ser reexecutados. A anotação cita hash join interno: se o hash não depende do parâmetro, pode ser reaproveitado quando permanece em uma única batch e sem o caso de NULLs que invalida essa reutilização.
- Cobrar startup/rescan como se todo o plano interno fosse reconstruído a cada chamada pode superestimar o custo; cobrar apenas o custo por tupla pode subestimá-lo. O custo deve seguir a dependência efetiva em `PARAM_EXEC` (`extParam` / `chgParam`).

### 2.3 Custo da igualdade parametrizada

Para uma condição interna como `inner.k = $1`, com `$1` vindo de uma Var externa:

- A estimativa atual anotada usa `var_eq_non_const`, aproximadamente `(1 - nullfrac_inner) / ndistinct_inner`.
- Isso trata o valor desconhecido como uniformemente provável entre os distintos.
- O comentário citado na captura questiona explicitamente se essa hipótese é razoável.
- Para comparação desigualdade com Param, a anotação registra fallback para `DEFAULT_INEQ_SEL`.

Esses são pontos de partida para revisão do código, não afirmações já revalidadas neste PR.

## 3. Item 3 — cache de SubPlan por valor de parâmetro

### 3.1 O que o executor já suporta

A investigação observou que um `Memoize` no topo do plano da subquery pode funcionar como cache por chave:

- `ExecScanSubPlan` sinaliza `chgParam` e chama `ExecReScan`.
- `ExecCacheMemoize` pode reiniciar o estado de lookup sem reexecutar o filho enquanto os parâmetros de chave não mudarem.
- Parâmetros fora da chave (níveis acima ou saída de initplan) limpam o cache inteiro; uma entrada incompleta, por leitura interrompida, é descartada e refeita.
- O modo binário compara os bits do datum e trata NULL com hash dedicado; a captura observa que isso evita depender de igualdade/collation não determinística.
- `finalize_plan` já trata `TMemoize`, ajustando `extParam` / `allParam`.

A oportunidade não é inventar um cache novo do zero, mas avaliar se a infraestrutura existente pode ser usada com segurança para o resultado de um SubPlan correlacionado.

### 3.2 Variante planejada

Depois de escolher o `best_path` da subquery, criar uma segunda variante memoizada quando:

- o SubLink for uma subquery escalar correlacionada por igualdade;
- os parâmetros de correlação puderem ser representados por chaves estáveis;
- o resultado for compatível com a semântica de uma entrada memoizada (incluindo zero/uma/múltiplas linhas e NULLs).

A captura sugere um helper conceitual `create_memoize_path(subroot, final_rel, best_path, param_exprs, NIL, singlerow, binary_mode = true, est_calls)`. Os `param_exprs` seriam os Params de `parParam`. A assinatura real e a compatibilidade com os tipos de SubLink precisam ser verificadas antes da implementação.

A variante memoizada e a normal seriam reunidas em `AlternativeSubPlan`. O join search não deveria pagar pela alternativa escolhida arbitrariamente: ambas precisam ter custos comparáveis ou a decisão deve ser postergada até haver informação suficiente.

### 3.3 Custo e decisão tardia

A variante precisa registrar no SubPlan o que o executor precisa para custeio posterior:

- (D): número estimado de valores distintos da chave;
- número estimado de chamadas;
- custo de miss (execução do filho);
- custo de hit (lookup);
- bytes por entrada e efeito de evicção/inserção.

O custo pode ser calculado em `setrefs`, quando `num_exec` e o contexto de execução são conhecidos. A hipótese anotada é usar `estimate_num_groups(root, param_exprs)` para estimar (D), mas a chamada direta no SubPlan pode não ter Vars e pode não carregar o contexto de distribuição correto. Isso precisa ser resolvido, não presumido.

Modelo de comparação simplificado:

```text
plain_cost  = calls * cost_per_call
memo_cost   = startup + D * cost_miss + (calls - D) * cost_hit
```

O modelo real também precisa incluir memória, tamanho da entrada, evicções, custo de hash/lookup e o comportamento quando (D) se aproxima de `calls`.

### 3.4 Fases, riscos e testes

**Fases propostas**

1. Gerar a variante e escolher em `setrefs`; medir primeiro com GUC experimental.
2. Corrigir a estimativa de `num_exec`; um qualificador seletivo pode reduzir as chamadas e tornar o plano normal melhor.
3. Opcionalmente, custear o cache em join search; a mudança na ordem de joins deve ser avaliada separadamente.

**Riscos registrados**

- Volatilidade: expressões voláteis não devem ser memoizadas sem prova de equivalência semântica.
- `random()` e `nextval()`: devem ficar fora.
- Paralelismo: cada worker teria seu próprio cache; o ganho pode cair.
- Memória: cache limitado por `hash_mem`, com política de evicção.
- Cardinalidade incorreta: resultados com mais de uma linha continuam sendo detectados; o cache não pode alterar a semântica.
- Filtros externos: a distribuição de valores observada pode diferir da distribuição marginal da coluna-base.

**Testes sugeridos**

- Chave de baixa cardinalidade e outer grande: `EXPLAIN` mostra Memoize no SubPlan.
- Chave única: sem Memoize.
- Expressão volátil: sem Memoize.
- Dois níveis de parâmetros externos: mudança do parâmetro externo invalida o cache.
- EXISTS, scalar e casos com NULL.
- Resultados corretos após purga do cache.
- Paralelismo e pressão de memória/evicção.

**Benchmarks indicados:** TPC-DS Q1, Q6, Q30, Q32, Q81 e Q92 para correlação por chaves de baixa cardinalidade; Q32/Q92 também aparecem na análise de custo por chamada. TPC-H, STATS-CEB e JOB devem ser usados como controles para confirmar que consultas sem o padrão relevante não mudam.

## 4. Item 2 — custo por chamada pela distribuição do parâmetro

### 4.1 Fórmula geral

Seja (P_{outer}(v)) a probabilidade de a entrada externa fornecer o valor (v), e (f_{inner}(v)) a quantidade de linhas internas correspondentes a esse valor. A cardinalidade média por chamada é:

[
E[inner\_rows\ per\ call] = \sum_v P_{outer}(v) \cdot f_{inner}(v)
]

Ela também pode ser expressa como a cardinalidade esperada do join por igualdade dividida pelas linhas externas:

[
E[inner\_rows\ per\ call] =
\frac{|outer \bowtie_{key} inner|}{|outer|}
]

Para custo, a forma geral é:

[
E[inner\_cost] = \sum_v P_{outer}(v) \cdot inner\_cost(v)
]

Se as distribuições externa e interna forem iguais e (f(v)) for a frequência relativa, a fração esperada de correspondências é proporcional a:

[
\sum_v f(v)^2
]

A expressão quadrática evidencia o efeito de skew quando a mesma distribuição governa tanto a seleção do parâmetro externo quanto a frequência de correspondências internas. Para distribuições diferentes, deve-se manter a fórmula geral; não se pode substituir as duas distribuições por uma só.

### 4.2 Correção importante: a distribuição relevante é a do ponto de avaliação

A captura corrige uma interpretação anterior sobre Q32/Q92:

- Em Q32/Q92, a Var externa pode ser `i_item_sk`, chave única em `item`. A distribuição dessa tabela-base é uniforme em (1/N); usá-la isoladamente reproduz aproximadamente a estimativa atual e não captura o skew.
- O enviesamento aparece na saída de `catalog_sales × item`, onde o qualificador correlacionado é avaliado. Cada linha externa chega na proporção das vendas daquele item; a distribuição relevante é a do conjunto externo já formado.
- Nesse caso, (P(v)) e a frequência interna podem ser proporcionais a (f_{cs}(v)), levando ao termo (sum f_{cs}(v)^2).
- Portanto, a estatística marginal da tabela-base não basta quando joins/filtros do lado externo mudam a distribuição.

Outro caso: na Q1, a Var externa vem de uma CTE com `GROUP BY`. O master descarta as estatísticas de frequência nesse caminho (a anotação cita o comentário “frequency-dependent stats must be adjusted”) e mantém apenas `ndistinct`; o valor esperado volta a ser uniforme. Assim, a fase 2a não deve prometer ganho para Q1. A oportunidade de Q1 pode estar no item 3 (cache), não na estimativa marginal do item 2.

### 4.3 Fase 2a — distribuição da coluna-base externa (protótipo localizado)

A primeira etapa é limitada ao caso em que o Param externo pode ser resolvido para uma Var simples de tabela-base.

**Helper proposto em `selfuncs.c`:**

```c
examine_outer_param(root, Param *, VariableStatData *, double *nd)
```

Comportamento proposto:

1. Aceitar apenas `PARAM_EXEC`.
2. Subir por `root->parent_root` procurando o `paramId` correspondente em `plan_params`.
3. Se não encontrar, tratar como initplan/outro caso e preservar o comportamento atual.
4. Exigir que a expressão externa seja uma Var simples; PHV e Aggref ficam fora da primeira versão.
5. Se o ancestral já tiver `simple_rel_array` (por exemplo, subquery LATERAL planejada no contexto do pai), usar o caminho completo de `examine_variable`.
6. Caso contrário, obter a relação pelo `rtable` ancestral usando `planner_rt_fetch`; verificar permissões com `all_rows_selectable` antes de ler estatísticas.
7. Obter `ndistinct` de `stadistinct` e `reltuples` / `variable_numdistinct`.

**Integração proposta:** em `eqsel_internal`, quando o argumento oposto for um Param resolvível, chamar a lógica de igualdade que compara as estatísticas das duas distribuições (na captura, associada a `eqjoinsel_internal`), reaproveitando o caminho de MCV/hash já existente. Sem estatísticas da coluna externa, manter exatamente o fallback atual.

**Limitações da fase 2a**

- Não incorpora filtros e joins externos.
- Para CTEs, manter o comportamento atual na primeira versão.
- Var não simples (PHV/Aggref) fica fora.
- Sem permissão para ler a coluna, não usar as estatísticas.
- Distribuição uniforme deve produzir planos byte-idênticos ou praticamente idênticos.

### 4.4 Fase 2b — distribuição no nível de avaliação (pesquisa)

Para obter a distribuição real do parâmetro no ponto em que o SubPlan é chamado, seria necessário considerar a relação externa já formada:

- localizar a EC da Var externa nas relações daquele nível;
- estimar a distribuição conjunta da chave no nível de avaliação, sob hipóteses explícitas de independência;
- combinar a seletividade do filtro e os joins que formam a entrada externa.

O momento natural seria após a formação do join tree, quando as relações e ECs existem. Porém, a subquery já foi planejada antes disso. Uma alternativa é re-custear após GUC ou gerar variantes planejadas sob duas distribuições e escolher mais tarde. Isso pode duplicar planejamento e alterar a ordem de joins, portanto é pesquisa separada, não parte do primeiro protótipo.

**Limitação:** a distribuição externa pode ser condicionada por filtros em outras colunas; sem estatísticas conjuntas adequadas, a estimativa continua sujeita à mesma limitação de correlação entre colunas.

## 5. Item 1 — número de chamadas

A tabela abaixo resume as mudanças anotadas. As referências de função/linha precisam ser confirmadas no commit-base.

| Subitem | Mudança proposta | Efeito esperado | Risco |
|---|---|---|---|
| 1a. Chamadas do qualificador | Usar as linhas de entrada do nó, não uma aproximação fixa `plan_rows * 2`; para scan, distinguir `rel->tuples` das linhas após filtros | Melhor estimativa de quantas vezes o qualificador roda | Baixo |
| 1b. Custo de rescan | Na subquery correlacionada, cobrar por chamada apenas startup/rescan dos nós que dependem de `parParam`; nós internos independentes podem ser reaproveitados | Evita cobrar repetidamente a construção de hash interno que não depende do parâmetro | Médio |
| 1c. Short-circuit | Cobrar qualificação cara somente quando ela pode ser avaliada após quals mais baratos do mesmo `security_level` | Evita cobrar avaliações que AND/OR não executam | Médio; respeitar ordenação e segurança |
| 1d. Regret de risco | Penalizar subqueries chamadas em nó cuja cardinalidade foi subestimada, usando o limite superior do plano maior | Reduz planos frágeis | Alto; sobrepõe-se à proposta geral de risco |

O subitem 1b depende de identificar corretamente quais nós têm dependência de parâmetro. O exemplo anotado é um hash interno construído uma vez e reutilizado; não se deve cobrar a construção do hash a cada chamada quando o nó não depende do parâmetro.

O subitem 1c deve respeitar a semântica de avaliação e a ordenação de quals por `security_level`. A anotação cita o comentário em `costsize.c` que explica por que seletividade não é simplesmente usada para ordenar todos os quals.

**Alternativa estrutural:** em vez de apenas mover um qualificador caro para cima de joins seletivos (*predicate migration*), converter uma subquery escalar agregada que sempre retorna uma linha em `JOIN LATERAL`. Isso daria ao join search liberdade para posicioná-la e poderia permitir `Memoize` em nested loop. É uma alternativa maior e separada.

## 6. Infraestrutura comum: resolver PARAM_EXEC

A função auxiliar de resolução do parâmetro externo deve ser compartilhada pelos itens 2a e 3.

**Contrato conceitual:** dado `PlannerInfo *root` e `Param *param`, encontrar a Var externa associada ao `paramId` no ancestral correto e devolver metadados necessários (Var, relação/atributo, estatísticas e ndistinct).

**Casos de teste indiretos**

- Param de initplan não resolve como Var externa.
- Param com dois níveis acima resolve no ancestral correto.
- RTE não relacional não resolve pelo caminho de tabela-base.
- Coluna sem permissão (`acl_ok = false`) não usa estatísticas.
- Var simples de tabela-base resolve.
- PHV/Aggref/CTE preservam o fallback da primeira versão.

## 7. Diagnóstico da regressão antes de implementar

1. Rodar no master e no build com as PRs as queries com SubPlan correlacionado:
   - escalares/agregadas: Q1, Q6, Q30, Q32, Q41, Q81 e Q92;
   - EXISTS/semi/anti: Q10, Q16, Q35, Q69, Q94 e Q95.
2. Gerar `EXPLAIN (PLAN_ADVICE)` no master e aplicar a advice no build com as PRs.
3. Comparar o custo do plano bom sob as estimativas novas e confirmar que a advice foi aplicada integralmente.
4. No plano ruim, localizar o nó mais baixo em que `estimated rows × loops` e `actual rows × loops` divergem por 10x ou mais.
5. Classificar:
   - outer do nested loop estimado perto de uma linha: cardinalidade;
   - custo por chamada errado: item 2, ou item 1b se houver hash interno reaproveitado;
   - loops muito acima do estimado: item 1;
   - chave repetida com poucos valores distintos: item 3 pode resolver independentemente da causa.

As listas de queries foram anotadas durante a investigação e devem ser conferidas no kit TPC-DS local antes de usar os resultados como evidência.

## 8. Marcos e critérios de aceite

| Marco | Entrega | Critério |
|---|---|---|
| M0 | Diagnóstico da regressão | Nó culpado identificado e classificado |
| M1 | `examine_outer_param()` + testes | Sem mudança de plano em casos sem estatística externa; parâmetros resolvidos no ancestral correto |
| M2 | Item 3: Memoize para SubPlan | Regressão verde; TPC-H e STATS-CEB byte-idênticos; TPC-DS muda apenas em queries com subquery correlacionada |
| M3 | Item 1a + 3.2 | Ganho medido em Q1/Q6/Q30/Q81, com rodadas alternadas; nenhuma regressão de tempo material |
| M4 | Item 2a | Estimativa de linhas por chamada dentro de aproximadamente 1,5x de `actual/loops` nos sintéticos; planos idênticos nos casos uniformes |
| M5 | Itens 1b/1c | Medidos isoladamente; cada um com GUC ou efeito restrito aos casos caros |
| M6 | Item 2b | Pesquisa em Q32/Q92; decidir se o ganho justifica a complexidade |

**Medição:** manter o protocolo de builds alternados (master/patch e ordem alternada), comparar planning time e execution time, e relatar também queries sem mudança. Não usar apenas q-error: o critério final é o efeito no plano e no tempo real.

## 9. Questões em aberto

- Confirmar no commit exato a fórmula de `num_exec` e o ponto em que é aplicada.
- Confirmar quais nós do SubPlan são cobrados como startup/rescan e quais podem ser reaproveitados.
- Confirmar se a semântica de SubPlan escalar permite a variante Memoize proposta em todos os casos pretendidos.
- Definir como estimar (D) sem perder a distribuição do parâmetro no nível correto.
- Determinar a API de catálogo/cache adequada para ler estatísticas da Var externa com segurança.
- Definir limites de memória e comportamento de evicção para Memoize.
- Verificar propostas upstream existentes para Memoize em SubPlan antes de abrir uma discussão upstream.

## 10. Ressalvas de evidência

Este documento consolida o conteúdo legível nas capturas e elimina trechos repetidos entre telas sobrepostas. Os números de linha, nomes de funções, comentários citados e caminhos de integração são pistas transcritas da sessão; não foram revalidados independentemente no commit do PostgreSQL nesta alteração. As fórmulas descrevem hipóteses de estimativa e devem ser testadas com distribuições uniformes, enviesadas e diferentes entre os lados. Nenhuma alteração de planner/executor é proposta como já implementada.
