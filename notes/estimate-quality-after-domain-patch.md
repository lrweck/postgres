# Qualidade das estimativas depois da regra de domínio

Medição com `bench/est_error.py` no build atual (domínio + #12/#13/#14), 123
queries, 7 puladas por estourar 30 s: 2.712 nós, 2.527 com `act > 0`.

## O que melhorou

Comparação com a pilha #5–#9 (baseline de 2.607 nós):

| nó | mediana antes | mediana agora | >10× antes | >10× agora |
|---|---|---|---|---|
| Hash Join | 6,03 | **1,38** | 47% | **11%** |
| Nested Loop | 22,38 | **2,03** | 66% | **29%** |
| Sort | 15,00 | **2,73** | 55% | 31% |
| Gather | 23,40 | **1,50** | 57% | **16%** |
| CTE Scan | 24,88 | 12,32 | 62% | 64% |
| Merge Join | 46,04 | 157,10 | 73% | 65% |

A regra de domínio acertou o que se propôs: a cardinalidade de join deixou de
ser o problema. Hash Join saiu de 6× para 1,4× de mediana; Nested Loop de 22×
para 2×.

## O que sobrou (por q-error × tempo)

| query | nó | estimado | real | q | tempo |
|---|---|---|---|---|---|
| DS14 | Hash Join `item ⋈ cross_items` | 1 | 16.193 | 16.193 | 9,3 s |
| DS14 | Nested Loop | 175 | 2.755.979 | 15.748 | 7,0 s |
| DS47 | Merge Join (CTE `v1` consigo mesma) | 1 | 35.512 | 35.512 | 1,5 s |
| DS57 | Merge Join (idem) | 1 | 23.870 | 23.870 | 0,9 s |
| DS23 | CTE Scan (`having count(*) > 4`) | 735.124 | 43 | 17.096 | 1,6 s |
| H18 | Sort (`having sum(l_quantity) > 300`) | 526.965 | 57 | 9.245 | 2,6 s |
| DS78 | Merge Join + filtro sobre `sum` | 22.651 | 101 | 224 | 2,9 s |
| DS38 | SetOp `intersect` | 146.479 | 111 | 1.320 | 0,6 s |
| DS45 | Nested Loop + filtro `OR ... IN (subquery)` | 18.979 | 16 | 1.186 | 0,1 s |

Nenhum desses é cardinalidade de join: são **seletividade de qual** ou
estatística que não existe.

- **DS14**: `item ⋈ cross_items` em três colunas `(brand, class, category)`.
  As colunas são funcionalmente dependentes; sem estatística estendida o
  modelo multiplica 1/1000 × 1/100 × 1/10 e chega a 1. É o caso da #13 — só
  dispara se o objeto existir, e o TPC-DS não tem nenhum.
- **DS47/DS57**: self-join de CTE em `(categoria, marca, loja, empresa)` +
  `d_year = d_year ± 1`. O estimador não reconhece a chave da CTE entre as
  duas referências. É o item 1.1.1 da nota de limitações.
- **DS23/H18/DS78**: filtro sobre agregado. `cost_agg()` aplica
  `clauselist_selectivity()` ao HAVING sem saber nada sobre a distribuição do
  tamanho dos grupos; o default é 1/3 (ou 1/2 no OU). Medido:

  | caso | média de linhas/grupo | default | real |
  |---|---|---|---|
  | `item` (1 linha por grupo), `count(*) > 1` | 1,0 | 6.000 | **0** |
  | `store_sales` por cliente, `count(*) > 4` | 46,3 | 20.725 | **90.857** |
  | DS23, `count(*) > 4` | 1,0 | 735.124 | **43** |
  | H18, `sum(l_quantity) > 300` | 4,0 | 526.965 | **57** |

  O default erra para os dois lados: 1/3 quando nenhum grupo passa e 1/3
  quando quase todos passam.
- **DS38**: `INTERSECT` estimado pelo maior input; a interseção real é 111.
  Precisa de estatística de conjunto, não existe.
- **DS45**: o join `web_sales ⋈ customer` está certo (36.324 contra 20.823
  reais); o erro é a seletividade do `OR (zip IN (...) OR item IN (subquery))`
  aplicada como join filter: 0,52 estimado contra 0,0008 real. Não é a regra
  de domínio.

## Conclusão

A estimativa de join está resolvida no nível que as estatísticas de coluna
permitem. O próximo ganho não está em mais uma regra de join: está em

1. **filtro sobre agregado** (HAVING), que é o caso comum e hoje usa 1/3 fixo;
2. estatística estendida para dependência multi-coluna (DS14) — opt-in, como
   já é a #13;
3. self-join de CTE (DS47/DS57), documentado desde a #11.

O item 1 é o único que dá para melhorar sem objeto novo: `cost_agg()` já tem
`input_tuples` e `num_groups`, então a média de linhas por grupo está na mão.
