# Estimativa de custo de correlated SubPlans

**Tipo:** nota de investigação / proposta de melhoria do planner  
**Data:** 2026-11-24  
**Status:** hipótese técnica a validar com protótipo e benchmarks  
**Escopo:** estimativa do número de chamadas, custo por chamada condicionado ao parâmetro e possível reutilização por valores distintos do parâmetro.

## Resumo

A estimativa de custo de uma subquery correlacionada envolve pelo menos três dimensões diferentes:

1. Quantas vezes o SubPlan será chamado.
2. Qual é o estimativa do custo de cada chamada, considerando a distribuição dos valores recebidos pelo parâmetro de correlação.
3. Quantos valores distintos do parâmetro serão observados e, portanto, quantas chamadas poderiam ser evitadas caso houvesse reutilização de resultados.

O planner já estima parcialmente a primeira dimensão, mas pode errar por não considerar adequadamente o comportamento do nó externo. A segunda dimensão parece uma oportunidade de melhoria usando estatísticas já existentes da coluna externa. A terceira exige suporte de execução/cache e não deve ser confundida com a estimativa de custo por chamada.

A ordem sugerida para investigação é: **(2) custo condicionado à distribuição do parâmetro; (3) reutilização por valores distintos; (1) refinamento do número de chamadas.**

## O que o planner sabe quando a subquery termina de ser planejada

Quando `make_subplan()` retorna, o planner mantém uma lista `plan_params`. Cada `PlannerParamItem` associa:

- a expressão/Var externa que fornece o valor;
- o slot `PARAM_EXEC` correspondente, usado para transportar o valor para dentro do SubPlan.

Assim, o planner sabe qual coluna externa alimenta cada parâmetro e qual expressão interna consome o `$n` — por exemplo, uma condição como `cs.item_sk = $1`.

A ligação entre o plano externo e o plano interno é feita pelo slot `PARAM_EXEC`. O que ainda não está necessariamente disponível no contexto do planejamento interno é o `RelOptInfo` do lado externo. As relações externas só são montadas em `query_planner()` (por exemplo, durante `setup_simple_rel_arrays()`).

As estatísticas das tabelas-base, contudo, já existem em `pg_statistic` e podem ser consultadas a partir da relação/atributo identificados pela Var externa. Isso sugere que parte da informação pode ser obtida sem esperar que o nível externo esteja completamente planejado.

## As três estimativas

### 1. Número de chamadas do SubPlan

O PostgreSQL já estima o custo por chamada do SubPlan. O valor `per_call_cost` é incorporado ao custo de avaliação e multiplicado pela estimativa de linhas do nó externo no ponto em que o SubPlan está anexado.

Em termos práticos, o número de chamadas é aproximado pela cardinalidade do nó externo que alimenta a expressão correlacionada.

Limitações apontadas na investigação:

- A estimativa de linhas do nó externo pode estar errada.
- O custo da subquery pode ser cobrado para todas as linhas de entrada, mesmo quando expressões booleanas e mecanismos de short-circuit podem evitar algumas avaliações.
- Se a estimativa de linhas do nível externo estiver baixa, a quantidade estimada de chamadas também ficará baixa.

Essa dimensão é diferente do custo de cada chamada e da quantidade de valores distintos do parâmetro.

### 2. Custo por chamada condicionado à distribuição do parâmetro

Esta é a oportunidade mais direta para um protótipo.

Dentro da subquery, uma condição como `cs.item_sk = $1` é estimada pelo caminho de igualdade com parâmetro não constante (`var_eq_non_const`). A estimativa genérica trata o valor ainda desconhecido como igualmente provável entre os valores distintos; em termos simplificados, usa aproximadamente `(1 - null_frac) / ndistinct`.

O comentário do próprio código questiona essa hipótese: o valor comparado ainda não é conhecido, então o planner assume que qualquer valor possível é igualmente provável, independentemente da frequência real desse valor na tabela. Isso ignora que os valores de `$1` vêm de uma coluna externa com distribuição própria.

**Fórmula para a cardinalidade média por chamada.** Seja:

- \(P_{outer}(v)\): probabilidade de uma linha externa fornecer o valor \(v\);
- \(f_{inner}(v)\): número de linhas internas que correspondem ao valor \(v\).

Então, o número esperado de linhas internas por chamada é:

\[
E[inner\_rows\ per\ call] =
\sum_v P_{outer}(v) \cdot f_{inner}(v)
\]

Isso também pode ser expresso como a cardinalidade esperada do join pela chave dividida pelo número de linhas externas:

\[
E[inner\_rows\ per\ call] =
\frac{|outer \bowtie_{key} inner|}{|outer|}
\]

A conexão com o estimador de join é importante: é essencialmente a estimativa de cardinalidade do join por igualdade dividida pelas linhas externas — a mesma área de estimativa que as PRs do usuário procuram melhorar. Para correlação por desigualdade, a hipótese anotada é usar a estimativa correspondente da PR #5.

**Fórmula para o custo médio por chamada.** Mais geralmente, o custo esperado deve ponderar o custo interno de cada valor pela probabilidade de esse valor ser fornecido pelo lado externo:

\[
E[inner\_cost] = \sum_v P_{outer}(v) \cdot inner\_cost(v)
\]

Se a distribuição externa e a interna forem a mesma, e \(f(v)\) representar a frequência relativa de cada valor, a fração esperada de correspondências é proporcional a:

\[
\sum_v f(v)^2
\]

Essa expressão evidencia o efeito do *skew*: valores frequentes têm maior probabilidade de chegar pelo lado externo e também tendem a produzir mais linhas no lado interno. Para distribuições diferentes, deve-se manter a fórmula geral ponderada, sem substituir as duas distribuições por uma só.

**Exemplo citado na investigação:** consultas TPC-DS como q32/q92, envolvendo `catalog_sales` e correlação por `item`. Cada linha externa fornece um item de acordo com a frequência de vendas desse item; a subquery consulta novamente linhas de `catalog_sales` para o mesmo item. Itens frequentes tendem a ser fornecidos mais vezes e também a produzir mais linhas na subquery. A média relevante é, portanto, ponderada pela distribuição externa, não uma escolha uniforme entre valores distintos.

#### Possível implementação

É possível investigar essa estimativa durante o planejamento da própria subquery:

1. Encontrar o `PARAM_EXEC` usado pela expressão interna.
2. Consultar `parent_root->plan_params` para recuperar a Var externa associada ao parâmetro.
3. Identificar a relação e o atributo da Var externa.
4. Obter as estatísticas da coluna externa por meio do catálogo/cache de estatísticas.
5. Calcular a seletividade/custo esperado ponderado pela distribuição externa, em vez de assumir apenas `1 / ndistinct`.

A ideia é fazer isso durante o planejamento, sem esperar que o nível externo esteja totalmente planejado. O ponto exato de integração e a disponibilidade de todos os dados ainda precisam ser confirmados no código.

#### Limitações

- A distribuição da coluna-base não incorpora automaticamente filtros e joins aplicados no nível externo.
- Exemplo: em q32, um filtro como `manufacturer_id = 977` pode alterar quais itens chegam à subquery. A distribuição marginal de `item_sk` não representa necessariamente a distribuição condicional depois desse filtro.
- Estatísticas multivariadas poderiam ajudar em alguns casos, mas não se deve presumir que resolvam toda a correlação entre joins e filtros.
- Se não houver estatísticas úteis para a coluna externa, o planner deve preservar o comportamento atual como fallback.
- Para uma chave externa uniforme, a estimativa não deveria mudar materialmente. O ganho esperado está em distribuições enviesadas (*skew*).

### 3. Valores distintos do parâmetro e reutilização de resultados

Uma terceira dimensão é a quantidade de valores distintos que o parâmetro de correlação assume.

A investigação observou que o executor de SubPlan não parece manter um cache geral de resultado por valor de parâmetro. O mecanismo `Memoize` existente atende a caminhos parametrizados de nested loop, o que não equivale a memoizar arbitrariamente o resultado de um SubPlan.

Em consultas como q1, q30 e q81, a correlação pode ocorrer por `store_sk` ou por estado, com poucos valores distintos. Nesses casos, reutilizar o resultado para parâmetros repetidos poderia reduzir substancialmente o número de execuções efetivas da subquery.

O planner pode já ter uma estimativa de grupos/valores distintos em alguns contextos (por exemplo, estimativas relacionadas a `estimate_num_groups`). Ainda faltaria conectar essa informação a uma estratégia de execução que efetivamente reutilize os resultados.

**Importante:** estimar poucos valores distintos não reduz, por si só, o custo real. É necessário um mecanismo de cache no executor, com semântica correta para parâmetros, invalidação/vida útil, memória e resultados. Essa parte exige desenho e avaliação próprios.

## Ordem recomendada de investigação

1. **Item 2 — custo condicionado à distribuição do parâmetro.** É relativamente localizado, usa estatísticas já existentes e pode ser prototipado sem alterar o executor. Na ausência de estatísticas da coluna externa, manter o fallback atual. Testar distribuição uniforme e distribuições enviesadas.
2. **Item 3 — reutilização por valores distintos.** Potencialmente relevante para q1/q30/q81, mas exige mecanismo de execução/cache; tratar como investigação separada.
3. **Item 1 — número de chamadas.** Refinar a cardinalidade do nó externo que alimenta o SubPlan. A estimativa já existe, mas herda erros do nível externo e pode não refletir short-circuit.

## Plano de validação

### Testes de estimativa

- Chave externa uniforme: a nova estimativa deve ficar próxima da estimativa atual.
- Chave externa enviesada: a estimativa deve refletir a média ponderada, aproximando-se do custo observado.
- Distribuição interna e externa diferentes: validar a fórmula geral \(\sum_v P_{outer}(v) \cdot inner\_cost(v)\), sem assumir que ambas as distribuições são idênticas.
- Estatísticas ausentes ou insuficientes: confirmar fallback seguro.
- Filtros externos: medir e documentar o erro introduzido por usar apenas a distribuição marginal da coluna-base.
- Comparar estimativa e execução em consultas TPC-DS relevantes, incluindo q32/q92 e q1/q30/q81, quando disponíveis.

### Métricas

- Linhas estimadas versus reais no nó externo que chama o SubPlan.
- Número estimado de chamadas versus chamadas observadas.
- Custo estimado por chamada versus linhas/trabalho efetivamente processados por chamada.
- Quantidade de valores distintos do parâmetro observada.
- Tempo total, planning time e execution time.
- Para eventual cache: taxa de hits, misses, memória usada e redução no número de execuções.

### Critério para aceitar a mudança

A melhoria não deve piorar materialmente casos uniformes ou sem estatísticas, deve reduzir o erro em casos enviesados representativos e deve ser validada com planos e tempos reais — não apenas com uma fórmula plausível.

## Questões em aberto

- Confirmar no código o ponto exato em que `parent_root->plan_params` está disponível e pode ser consultado com segurança.
- Confirmar a API apropriada para obter as estatísticas da Var externa no contexto do planner.
- Definir como combinar MCVs, histogramas e valores fora da lista MCV sem introduzir viés adicional.
- Avaliar se estatísticas estendidas podem ser usadas quando a distribuição externa é condicionada por filtros.
- Verificar se existe proposta upstream prévia para memoização de SubPlans correlacionados.
- Definir a semântica e o orçamento de memória de eventual cache no executor.

## Proveniência e ressalvas

Esta nota organiza a análise técnica exibida em uma captura de tela de uma sessão de investigação. As referências a funções, estruturas e consultas são pistas para orientar a revisão do código; não constituem, por si só, confirmação independente da implementação atual. Antes de propor patch upstream, validar cada afirmação no branch e na versão exata do PostgreSQL em estudo, e registrar benchmarks reproduzíveis.
