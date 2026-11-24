# PostgreSQL 19: JIT desabilitado por padrão

**Tipo:** nota técnica / investigação de performance  
**Data:** 2026-11-24  
**Escopo:** mudança de default no PostgreSQL 19 e impacto em workloads OLTP e OLAP.

## Resumo

O PostgreSQL 19 mudou o parâmetro global `jit` de `on` para `off`. Isso significa que o JIT não será usado por padrão, mesmo quando o custo estimado de uma consulta ultrapassar `jit_above_cost`.

A mudança **não** significa que o PostgreSQL passou a aplicar JIT em consultas pequenas, nem que o threshold foi removido. Com `jit = on`, a decisão continua baseada nos thresholds de custo. Com `jit = off`, nenhum desses thresholds habilita o JIT.

## Como a decisão funciona

Quando o JIT está habilitado, o PostgreSQL compara o custo estimado do plano com os parâmetros:

- `jit_above_cost`: custo mínimo estimado para ativar JIT.
- `jit_inline_above_cost`: custo mínimo para permitir inlining de funções e operadores.
- `jit_optimize_above_cost`: custo mínimo para aplicar otimizações mais caras.

Essas decisões são tomadas no planejamento, não no momento da execução.

Consequência prática:

| Configuração / custo estimado | Resultado |
|---|---|
| `jit = off` | JIT não é usado, independentemente do custo |
| `jit = on`, custo abaixo de `jit_above_cost` | Sem JIT |
| `jit = on`, custo acima de `jit_above_cost` | JIT pode ser usado |
| Custo acima dos thresholds de inlining/otimização | Recursos adicionais de JIT podem ser habilitados |

Portanto, em PostgreSQL 18 com `jit = on`, uma consulta abaixo de `jit_above_cost` não sofria o overhead de compilação JIT. A mudança do PostgreSQL 19 afeta principalmente consultas que cruzariam o threshold, mas agora não recebem JIT porque o parâmetro global vem desligado.

## Por que o default mudou

A justificativa do commit é que o modelo de custos usado para decidir quando compilar não estima de forma confiável o custo da compilação e otimização LLVM.

Isso pode produzir um *performance cliff*: alterações aparentemente não relacionadas — por exemplo, algumas inserções adicionais — elevam o custo estimado o suficiente para cruzar o threshold. O plano passa a disparar JIT, embora a consulta real continue curta; o custo de compilação pode superar o benefício obtido durante a execução.

O commit também observa que grandes deployments já costumavam desabilitar JIT por padrão. O default foi alinhado a essa prática, deixando aberta a possibilidade de reabilitá-lo por padrão no futuro, caso o modelo de custos melhore ou o custo de compilação diminua.

## Implicações

### OLTP

- O default conservador evita que consultas curtas tenham latência elevada por compilação inesperada.
- Reduz a possibilidade de regressões abruptas causadas por estimativas que cruzam o threshold.
- Consultas abaixo do threshold já não recebiam JIT quando `jit = on`; não se deve atribuir à mudança um ganho direto em todas as queries rápidas.

### OLAP

- Consultas analíticas longas e CPU-bound ainda podem se beneficiar de JIT.
- Ambientes com esse perfil devem avaliar habilitar `jit = on` explicitamente.
- Não se deve presumir que habilitar JIT melhora toda consulta analítica: é necessário medir o custo de compilação e o tempo total.

## Prepared statements

Como as decisões baseadas em custo são tomadas no planejamento, quando prepared statements usam um plano genérico, os parâmetros de configuração em vigor na preparação controlam essas decisões, e não necessariamente os valores vigentes no momento da execução.

## Plano de validação / benchmark

Comparar pelo menos estas configurações no mesmo workload e ambiente:

1. **A — Default PG19:** `jit = off`.
2. **B — JIT habilitado:** `jit = on` com thresholds padrão.
3. **C — JIT habilitado com thresholds ajustados:** apenas para investigação de sensibilidade; não assumir que thresholds menores são melhores.

Coletar:

- `Planning Time` e `Execution Time` do `EXPLAIN (ANALYZE, BUFFERS, SETTINGS)`.
- Seção `JIT` do plano, incluindo tempo de geração, inlining, otimização e emissão.
- Latência total em execuções repetidas: p50, p95 e p99.
- Resultados separados para consultas curtas/OLTP e consultas longas/CPU-bound.
- Resultados com e sem prepared statements, incluindo plano genérico quando aplicável.

Evitar comparar apenas a primeira execução: separar efeitos de cache, aquecimento e compilação. Manter os dados, estatísticas, parâmetros e concorrência equivalentes entre os cenários.

## Conclusão

A resposta à dúvida central é: **não, queries abaixo de `jit_above_cost` não eram compiladas por JIT apenas por ele estar habilitado no PostgreSQL 18**. O que mudou no PostgreSQL 19 foi o default global `jit = off`. Os thresholds continuam relevantes quando o JIT é explicitamente habilitado.

A decisão é especialmente defensiva para OLTP. Para OLAP, a configuração deve ser validada com benchmarks representativos.

## Referências

1. Commit upstream: [jit: Change the default to off](https://git.postgresql.org/pg/commitdiff/7f8c88c2b872cb74882ab93dcb05529dab2a10bc).
2. Discussão do commit na lista pgsql-committers: [pgsql: jit: Change the default to off](https://www.postgresql.org/message-id/E1w8GWU-002bSL-31%40gemulon.postgresql.org).
3. Documentação PostgreSQL 19: [When to JIT?](https://www.postgresql.org/docs/19/jit-decision.html).
4. Notas de lançamento PostgreSQL 19: [Release 19](https://www.postgresql.org/docs/current/release-19.html).
