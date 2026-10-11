# Étape 3 : simulation (`simulation`)

Faire évoluer l'île de l'étape 2 dans le temps, sur la grille grossière :
`simulation.total_duration_years` par pas de `simulation.dt_years`. Cette page couvre la
**partie 1** (la boucle temporelle, le soulèvement, le routage de l'eau et l'érosion
fluviale), la **partie 2** (l'érodabilité selon la roche, les sédiments et les versants,
§ 9 à 13) la **partie 3** (le dépôt fluvial, les deltas et la diffusion marine, § 14 à 18), des
correctifs d'équilibre entre soulèvement et érosion (§ 19 à 21) et la **partie 4** (le climat
et l'érosion marine, § 22 à 27).

← [Étape 2](etape-2-initialisation.md) · [Vue d'ensemble](README.md)

Code : [SimulationStage.cpp](../src/wg/gen/stages/SimulationStage.cpp), et
[src/wg/sim/](../src/wg/sim/) pour le routage ([Routing](../src/wg/sim/Routing.hpp)), les
paramètres temporels ([TimeParams](../src/wg/sim/TimeParams.hpp)), les versants
([Hillslope](../src/wg/sim/Hillslope.hpp)), le transport fluvial
([Fluvial](../src/wg/sim/Fluvial.hpp)) et la diffusion marine
([Marine](../src/wg/sim/Marine.hpp)).

---

## 1. La boucle

```
pour chaque pas (t → t + dt, évalué au milieu du pas) :
    1. paramètres temporels : niveau marin(t), activité de chaque phase
    2. soulèvement                       simulation.processes.uplift
       puis la roche au sommet de b, réévaluée (§ 9)
    3. climat (tous les climate_update_interval_steps pas) : T, P orographique,
       débit pondéré par la pluie         simulation.processes.climate
    4. glaciers                          (partie 5)
    5. routage, érosion et dépôt fluvial simulation.processes.fluvial
       deltas aux embouchures
       la mer du pas : l'océan du routage, figé jusqu'à la fin du pas
    6. versants : diffusion, effondrement simulation.processes.hillslope
    7. érosion par les vagues (falaises), puis diffusion marine des sédiments
                                         simulation.processes.marine
    8. bilan, contrôles, logs, aperçu en direct
```

Chaque processus a son flag dans `simulation.processes`. Ceux qui ne sont pas encore écrits
le signalent une fois dans le log, puis sont sautés.

**État simulé**, en `double` pendant toute l'étape :
- `b`, le sommet de la roche ;
- `sed`, l'épaisseur de sédiments, et sa composition `sed_frac` (gravier, sable, argile) ;
- `h = b + sed`.

Le `double` est indispensable pour ε : en `float`, deux valeurs voisines sont espacées
d'environ 10⁻⁴ m dès 1 km d'altitude, donc `h + 1e-4` vaudrait `h`. Les calques `float` ne
sont écrits qu'aux aperçus et à la fin.

| Calque | Contenu |
|--------|---------|
| `h`, `bedrock` | surface et sommet de la roche (m) |
| `discharge` | débit `Q` (m³/an), affiché en échelle log |
| `lake_depth` | profondeur des dépressions remplies (m) |
| `erosion_rate` | érosion fluviale du dernier pas (m/an) |
| `cum_uplift` | soulèvement cumulé total (relief) |
| `strata_uplift` | soulèvement qui déplace les strates (sans les phases à failles, voir § 3) |
| `surface_material` | roche au sommet de `b` (sous les sédiments), mise à jour à chaque aperçu |
| `sediment` | épaisseur des sédiments (m), en échelle log (5 décades sous le maximum) |
| `sediment_flux` | flux de sédiments fluvial `Qs` sortant de la cellule (m³/an), en échelle log |
| `deposition_rate` | dépôt fluvial du dernier pas (m/an), deltas compris, en échelle log |
| `temperature` | température à la surface `h + glace` (°C) |
| `precipitation` | précipitations orographiques (m/an) |
| `coast_exposure` | exposition des cellules de côte à la houle, [0, 1] |
| `coast_wear` | usure accumulée par les vagues / volume de la cellule, [0, 1] |
| `sediment_gravel`, `sediment_sand`, `sediment_clay` | fractions de chaque sorte dans les sédiments |
| `sediment_dominant` | sorte dominante (`none` sous 1 mm de sédiments) |

Scalaires de l'état : `time_years`, `step`, `steps`, `phase_progress_k`, `sea_level_m`
(niveau marin courant, relatif au niveau de référence ; le viewer l'affiche sous le temps).

## 2. Paramètres temporels ([TimeParams](../src/wg/sim/TimeParams.cpp))

- **Niveau marin :** `niveau(t)` est l'interpolation linéaire de `timeline.sea_level_curve`,
  ajoutée au niveau de la mer actuel (0 si la courbe est vide).
- **Activité d'une phase :** un trapèze sur `[start_years, end_years]`, avec des rampes de
  5 % de la durée de la phase.

## 3. Soulèvement

```
U(x, y, t) = Σ_k rate_k · P_k(x, y) · activité_k(t)
b += U · dt ,  cum_uplift += U · dt          (sauf cellules FIXED)
```

- **Strates.** Les phases à failles (`block_uplift`, `horst_graben`) déplacent leurs
  strates par leurs blocs ([étape 2, § 3.6](etape-2-initialisation.md)). La simulation fait
  avancer leur part (`phase_progress_k`, soit l'intégrale de l'activité divisée par son
  total), et leur soulèvement n'entre pas dans `strata_uplift`. Sinon, ces strates
  monteraient deux fois.

## 4. Routage de l'eau ([Routing](../src/wg/sim/Routing.cpp))

1. **Exutoires :** les cellules `FIXED`, plus l'**océan**, c'est-à-dire les cellules sous le
   niveau marin connectées au bord (remplissage en 8-connexité).
2. **Priority-Flood + ε** (Barnes, Lehman & Mulla 2014), depuis les exutoires qui bordent la
   terre, avec la file de dépressions de Barnes. Ce passage donne deux surfaces :
   - `h_fill ≥ h`, avec une pente de ε = 10⁻⁴ m par cellule dans les dépressions, qui ne
     sert **qu'au routage** ;
   - le **plan d'eau** `water`, horizontal dans un lac (le niveau du déversoir) et égal à `h`
     ailleurs. On en déduit `lake_depth = water − h`.

   Mesurer les lacs sur `h_fill` ajouterait ε à chaque cellule traversée : sur un plateau de
   300 cellules, 3 cm, de quoi prendre tout le plateau pour un lac.
3. **Receveurs :** la plus forte pente `(h_fill_i − h_fill_j) / L` sur `h_fill`, avec
   `L = dx` ou `dx·√2`.
   - **Hors lac,** `h_fill = h` : c'est la vraie plus forte pente.
   - **Dans un lac,** la pente ε mène l'eau à travers le lac jusqu'au déversoir.
   - **Existence garantie :** toute cellule qui n'est pas un exutoire a un voisin strictement
     plus bas sur `h_fill` (son parent dans le Priority-Flood).
4. **Pile** (Braun & Willett 2013) : liste des donneurs, puis parcours en profondeur depuis
   chaque exutoire. Chaque cellule y apparaît après son receveur. L'ordre de dépilement du
   Priority-Flood ne le garantit pas pour des receveurs de plus forte pente.
5. **Débit :** `Q = P · dx²` par cellule (P = `climate.base_precipitation_m_per_year`,
   constant dans cette partie), accumulé en parcourant la pile à l'envers.

## 5. Érosion fluviale (loi de puissance implicite, Braun & Willett 2013)

`∂b/∂t = U − K · Q^m · S^n`, avec `n = 1` imposé (avertissement si la config dit autre
chose). On parcourt la pile du niveau de base vers l'amont :

```
F   = K · Q^m · dt / L(i, receveur)
h_i = (h_i + F · h_base) / (1 + F)      h_base : nouvelle altitude du receveur
```

`K` dépend de ce qui affleure (§ 9) : sédiments ou roche.

Le schéma est inconditionnellement stable (validé avec `dt = 10 000 ans`). Quelques règles
s'ajoutent :

- **Cellules exclues :** les cellules marines et `FIXED` n'érodent pas.
- **Niveau de base** d'une cellule : le **niveau marin** si son receveur est en mer ; le
  **plan d'eau** si son receveur est dans un lac ; sinon la nouvelle altitude du receveur.
  Une cellule sous son niveau de base n'érode pas, et le dépôt viendra avec les sédiments.
- **Lacs :** une cellule de lac est sous le plan d'eau, donc elle n'érode jamais, quelle que
  soit sa profondeur. Rien ne peut creuser sous la surface d'un lac. Avec un seuil de
  profondeur, les bords peu profonds d'une cuvette s'érodaient vers son fond et la
  creusaient.
- **Vérification :** en build Debug, la simulation vérifie qu'aucune cellule ne passe sous son
  niveau de base ni sous la mer.

**Relief initial.** Sur une surface parfaitement lisse, le routage D8 donne des rivières
parallèles dans les 8 directions de la grille : rien n'amorce les captures. Un relief
initial rugueux de quelques mètres suffit à faire émerger un réseau dendritique (voir
[configs/validation_dome.jsonc](../configs/validation_dome.jsonc)).

## 6. Contrôles et logs

- **Valeurs non finies :** arrêt au premier NaN ou infini, avec un dump de l'état dans
  `debug.cache_dir/simulation_nan_step_N.wgw` et le numéro du pas.
- **Cellules `FIXED` :** vérifiées inchangées à chaque log.
- **Log :** tous les `simulation.log_interval_steps` pas, une ligne avec le temps simulé,
  `max(h)`, `mean(|dh/dt|)` sur la terre, le volume de sédiments, l'erreur de volume des
  versants (§ 12), le nombre de sous-pas ADI et le temps par pas. `wgen` l'affiche (mode batch,
  qui écrit l'état final), et le viewer en montre la dernière ligne.

**Performance.** Partie 1 seule : environ 40 à 65 ms par pas à 1024² sur un Mac récent,
selon la surface de terre. Pour l'essentiel, c'est le Priority-Flood ; seuls les exutoires
en bordure de terre y entrent. La partie 2 ajoute la roche (environ 15 ms), l'effondrement
(20 à 40 ms par passe) et surtout la diffusion : environ 8 ms par sous-pas ADI, avec
`ceil(D · dt / dx²)` sous-pas (§ 10). À `D = 0,09`, `dt = 4 700 ans` et `dx = 4 m`, cela
fait 27 sous-pas, et un pas de 300 à 450 ms.

## 7. Dans le viewer

| Touche | Action |
|--------|--------|
| `Espace` | met la simulation en pause ou la relance |
| `N` | avance d'un pas (et met en pause) |
| `+` / `−` | vitesse : max, 100, 30, 10, 3 ou 1 pas/s |

Le panneau affiche le temps simulé et le numéro de pas. En pause ou pas à pas, chaque pas est
publié. Les couches les plus utiles sont `discharge` (le réseau de rivières), `erosion_rate`
et `lake_depth`.

## 8. Validation

[configs/validation_dome.jsonc](../configs/validation_dome.jsonc) et les tests
(`testSimulationSteadyState`, `testSimulationLake`) vérifient :

- **Équilibre :** sous un dôme constant, `mean |dh/dt|` décroît vers 0.
- **Réseau :** il se concentre, le débit maximal dépassant de loin celui d'une cellule.
- **Calibration :** doubler `k_base` donne un relief d'équilibre plus bas.
- **Lac :** une cuvette creusée devient un lac que le drainage traverse.
- **Robustesse :** déterminisme, aucun NaN, cellules `FIXED` intactes, `dt = 10 ka` stable.

---

# Partie 2 : roches, sédiments, versants

## 9. Érodabilité selon la roche et les sédiments

Après le soulèvement, chaque pas réévalue la **roche au sommet de `b`** (et non de `h`, qui
inclut les sédiments) avec la géologie de l'étape 2 :
`Geology::material(x, y, b, strata_uplift, z_ref, fold_offset)`, les phases à failles étant
avancées à leur progression courante. On en tire, par cellule, l'érodabilité, l'angle de
talus et la sorte de sédiment que donne la roche (`materials[].sediment`, qui doit valoir
`gravel`, `sand` ou `clay`). Le calcul est parallélisé par lignes.

```
sed_i > 0 :  K_i = k_base · erosion.fluvial.k_sediment_mult   (défaut 5)
sinon     :  K_i = k_base · materials[roche_i].erodibility
```

**Découpe du pas** quand l'érosion traverse la couverture. Le schéma implicite est un
rapprochement du niveau de base : `érosion = chute · F / (1 + F)`, avec
`chute = h_i − h_base`. On calcule d'abord l'érosion au taux sédiment ; si elle dépasse
`sed`, on découpe le pas en temps :

```
F_sed · τ = sed / (chute − sed)          part τ du pas qui décape la couverture (même schéma implicite)
F'        = F_roche · (1 − τ)            le reste du pas, au taux roche
érosion   = sed + (chute − sed) · F' / (1 + F')
```

Chaque partie est un pas implicite vers le même niveau de base : le résultat reste stable et
ne passe jamais sous ce niveau (la vérification Debug du § 5 s'applique toujours).
L'érosion est prise sur `sed` puis sur `b`. Depuis la partie 3, ce qui est érodé est
transporté et déposé (§ 14).

## 10. Diffusion de versant (ADI implicite)

`∂h/∂t = D · ∇²h`, avec `D = erosion.hillslope.diffusivity_m2_per_year`, **sur la terre
seulement** (`h > niveau marin`, hors `FIXED` et hors mer du pas). La mer est une **limite
absorbante, au niveau marin** : pendant la résolution, toutes les cellules marines valent le
niveau de la mer, et elles absorbent ce qui franchit la côte. Ce volume est déposé en
sédiment dans la cellule marine, puis étalé par la diffusion marine (§ 16). Les cellules
`FIXED` sont des murs.

**Pourquoi le niveau marin, et pas la profondeur de chaque cellule.** Avec chaque cellule
marine à sa propre profondeur, une cellule de côte entre une mer peu profonde et une mer
profonde servait de conduit. À chaque sous-pas, elle faisait passer du sédiment de la
première (creusée jusque dans sa roche) vers la seconde. Avec 27 sous-pas (`dx` = 4 m),
le transfert s'emballait, et la seed 3 a atteint 1e150 m en quelques centaines de pas. Au
niveau marin, la terre reste au-dessus de la limite : elle ne fait que perdre vers la mer.
Une embouchure surélevée par son dépôt ne reverse pas non plus sa charge sur la côte (test
`testStrongDiffusionStaysBounded`, vérification Debug : la mer ne rend jamais de volume).

**Schéma de Peaceman–Rachford**, avec `r = D · dt / dx²` :

```
demi-pas x :  (1 − r/2 · δx²) h*   = (1 + r/2 · δy²) hⁿ      une tridiagonale par ligne
demi-pas y :  (1 − r/2 · δy²) hⁿ⁺¹ = (1 + r/2 · δx²) h*      une tridiagonale par colonne
```

- **Tridiagonales :** chaque suite de cellules de terre consécutives d'une ligne forme un
  système indépendant, résolu par l'algorithme de Thomas. Un mur au bout ne laisse rien
  passer ; une cellule marine au bout est une hauteur imposée, et le flux qu'elle reçoit est
  compté.
- **Parallélisme :** les lignes sont indépendantes et réparties sur un pool de threads
  persistant ([Parallel](../src/wg/core/Parallel.hpp)).
- **Lecture contiguë :** le demi-pas y travaille sur la grille transposée (transposition par
  blocs).

**Sous-pas.** Peaceman–Rachford est inconditionnellement stable, mais pour un très grand `r`
le facteur d'amplification des petites longueurs d'onde tend vers ±1 : le bruit à l'échelle
de la maille (un chenal d'une cellule, par exemple) n'est presque plus amorti, et la partie
explicite crée des oscillations. Le pas est donc découpé en `ceil(r)` sous-pas de `r ≤ 1` :
la partie explicite reste monotone (aucun nouvel extremum) et le bruit de maille est amorti
d'un facteur 9 par sous-pas. C'est le poste le plus coûteux de la simulation (§ 6). Si c'est
trop lent, un schéma implicite par direction (Euler implicite en x puis en y), monotone en un
seul pas mais du premier ordre en temps, serait l'alternative.

**Comptabilité.** La variation `Δh` de chaque cellule (terre ou mer absorbante) :
- **`Δh < 0` :** prélevée sur `sed`, puis sur `b` ;
- **`Δh > 0` :** ajoutée à `sed`. Sa composition est celle de ce qui a été prélevé dans les
  5×5 cellules autour. À défaut, c'est celle de la couverture de la cellule ou de sa roche.
  C'est une approximation : la diffusion ne suit pas l'origine de chaque volume.

## 11. Effondrement thermique (Musgrave, Kolb & Mace 1989)

`erosion.thermal.iterations_per_step` passes par pas, chacune en **double buffer** : tous les
transferts sont calculés à partir des hauteurs courantes, puis appliqués. Il n'y a donc aucun
biais d'ordre.

```
pour une cellule de terre i et ses 8 voisins j (hors FIXED), L_ij = dx ou dx·√2 :
    excès_j(angle) = h_i − h_j − L_ij · tan(angle)
    dépl_sed   = min(sed_i, rate · max_j excès_j(angle des sédiments))
    dépl_roche = rate · max_j excès_j(talus_angle_deg de la roche de i)
    déplacé    = max(dépl_sed, dépl_roche)
    réparti entre les voisins au prorata de leur excès (à l'angle qui gouverne)
```

- **`rate`** (`erosion.thermal.rate`, 0,5 par défaut) : la moitié de l'excès, comme demandé.
- **Couverture et roche.** Une couverture de sédiments glisse à
  `erosion.thermal.sediment_talus_angle_deg` (33° par défaut), mais au plus son épaisseur.
  La roche en dessous ne cède qu'au-delà de son propre angle. Prendre l'angle des sédiments
  dès qu'il y en a un millimètre ferait effondrer une paroi de granite sous une fine
  pellicule.
- **Matière déplacée :** elle vient de `sed`, puis de `b`. Elle arrive en `sed` chez les
  voisins (éboulis), avec sa composition : celle de la couverture, plus la sorte de sédiment
  de la roche pour la part rocheuse. Les receveurs peuvent être en mer : une falaise s'éboule
  dans l'eau.
- **Stabilité :** les transferts sont simultanés, donc une cellule au pied de plusieurs
  voisins très hauts recevrait la moitié de l'excès de chacun et les dépasserait. Avec de
  grands dénivelés, cette oscillation s'amplifie (elle a fait diverger une simulation, avec
  des hauteurs de 1e30 m). Une cellule reçoit donc au plus la moitié du plus grand excès de
  ses donneurs sur elle, et les transferts vers elle sont réduits d'autant. Les volumes
  restent exacts, puisqu'on ne fait que réduire des transferts.
- **Convergence :** la matière ne descend que d'une cellule par passe. Une falaise de 100 m
  demande quelques centaines de passes pour atteindre son angle (test
  `testThermalCollapse`). En simulation, l'incision raidit à nouveau des berges à chaque pas,
  donc une passe par pas laisse des pentes un peu au-dessus de la limite juste après
  l'incision (voir § 12).

## 12. Composition et contrôles

**Composition.** Chaque cellule porte `sed_frac`, les fractions de gravier, de sable et
d'argile. Un dépôt les mélange au prorata des volumes :
`frac' = (sed · frac + volumes) / (sed + Σ volumes)`. Ce qui est prélevé dans la couverture
garde sa composition, et ce qui est prélevé dans la roche prend la sorte de sédiment de son
matériau.

**Contrôles.**
- **Volume :** la diffusion et l'effondrement ne font que déplacer de la matière.
  L'erreur relative `|Σ Δh| / Σ |déplacements|` est loguée (`hillslope dV/V`). Elle reste
  de l'ordre de 1e-15 (précision du `double`) ; on somme les variations par cellule, car
  la différence de deux sommes de hauteurs (environ 1e7 m sur une grille) noierait l'erreur
  dans l'arrondi.
- **`sed ≥ 0` :** garanti par la comptabilité, et vérifié en build Debug.

## 13. Validation de la partie 2

| Config / test | Attendu | Mesuré |
|---------------|---------|--------|
| [validation_folds.jsonc](../configs/validation_folds.jsonc) | crêtes de roche dure, vallées de roche tendre, bandes de `surface_material` dans l'axe des plis | relief local moyen : gneiss +3,1 m, shale −2,5 m ; bandes visibles |
| [validation_old_mountains.jsonc](../configs/validation_old_mountains.jsonc) | reliefs arrondis après l'arrêt du soulèvement | collines émoussées, aucune pente au-dessus de son angle |
| `testThermalCollapse` | éboulis au pied, pentes ≤ angle limite | roche ≤ 1,1 × 45°, éboulis ≤ 1,15 × 33° |
| `testHillslopeDiffusion`, `testSimulationSediment` | volume conservé, `sed ≥ 0`, fractions de somme 1 | erreur de volume ≈ 1e-15 |

**Limites connues.**
- **Bourrelet côtier** (résolu à la partie 3 : la côte absorbe le flux de la diffusion). Le
  bord de la terre était sans flux, donc la diffusion accumulait du sédiment le long de la
  côte.
- **Pentes juste après l'incision.** Avec une passe thermique par pas, environ 7 % des
  cellules de roche nue dépassent leur angle de plus de 5 % à la fin d'un pas (2 % avec 5
  passes). Ce sont des berges que la rivière vient de raidir.

---

# Partie 3 : dépôt fluvial, deltas, diffusion marine

## 14. Loi de puissance avec dépôt (Yuan, Braun, Guerit et al. 2019)

```
∂h/∂t = U − K · Q^m · S + G · Qs / A
```

- **`Qs`** (m³/an) est le flux de sédiments qui entre dans la cellule depuis l'amont.
- **`A`** est l'aire drainée, et **`G = erosion.fluvial.deposition_g`** est sans dimension.

Le dépôt `G · Qs / A` (en m/an) est la forme volume par surface du `(G / (p·A)) · Qs` de
l'article. Avec une pluie uniforme `p`, c'est le `(G / Q) · Qs` de la formulation, multiplié
par `p`. **Le cas `Q` petit ne pose pas de problème** : `A` vaut au moins une cellule, et
`Qs` est nul en tête de bassin. Le dépôt est en outre plafonné par ce qui entre dans la
cellule.

**Schéma implicite, Gauss–Seidel sur `Qs`**
([Fluvial.cpp](../src/wg/sim/Fluvial.cpp)), comme dans l'article. Chaque itération fait
deux choses :
1. Elle résout `h` du niveau de base vers l'amont, avec le `Qs` de l'itération précédente :

   ```
   dépôt  = min(G · Qs · dt / A, Qs · dt / dx²)
   h'     = h + dépôt                        // le dépôt rejoint la couverture
   érosion implicite de h' vers le niveau de base, avec la découpe sédiment / roche du § 9
   ```
2. Elle recalcule `Qs` en descendant la pile inversée (de l'amont vers l'aval) :
   `Qs_aval += Qs_entrant + (h_avant − h_après) · dx² / dt`.

On s'arrête quand la variation relative de `Qs` (norme L1) passe sous
`erosion.fluvial.deposition_tolerance` (1e-3), ou après
`erosion.fluvial.deposition_max_iterations` (20). En pratique, 3 à 5 itérations suffisent.
**Avec `G = 0`**, une seule passe suffit et l'on retrouve exactement le schéma de la
partie 1 (test `testFluvialRegression`).

**Passe finale.** Une dernière descente de la pile applique la variation de chaque cellule :
- **un dépôt** prend la composition du flux, au prorata ;
- **une érosion** est prise sur `sed` puis sur `b`, et sa composition s'ajoute au flux.

Le flux transporte ainsi sa composition, mélangée par volume le long de la pile. Le dépôt y
est plafonné par ce qui entre réellement : le bilan est exact par construction.

**Lacs.** Les cellules de lac (`plan d'eau > h`) gardent tout ce qui entre, jusqu'à remplir
le lac. Le flux traverse le lac le long des receveurs et le comble à partir de son entrée :
les lacs se remplissent progressivement (test `testLakeFilling`).

## 15. Deltas : les sédiments qui arrivent en mer

Le `Qs` qui atteint une embouchure remplit la mer **jusqu'au niveau marin**, de proche en
proche depuis l'embouchure : un parcours en largeur dans les cellules marines, limité à
4 096 cellules par embouchure. Ce qui reste est réparti en couche uniforme sur les cellules
parcourues, c'est-à-dire sur le haut du delta.

**Pourquoi pas tout dans la cellule d'embouchure.** La charge d'un pas atteint des millions de
m³, soit des milliers de mètres de colonne sur une cellule de 4 m. Il fallait une diffusion
marine de l'ordre de 100 m²/an pour l'étaler, et sinon le monticule dépassait la mer,
devenait de la terre et barrait la rivière. On obtenait un lac, qui se remplissait, que la
rivière recreusait, et ainsi de suite. Le remplissage jusqu'au niveau marin donne au
contraire un delta qui avance, et la diffusion marine en façonne le front.

**La mer du pas.** C'est l'océan trouvé par le routage (cellules sous le niveau marin
reliées au bord). Elle reste fixe jusqu'à la fin du pas : une embouchure surélevée par son
dépôt reste une cellule marine pour les versants et pour la diffusion marine.

## 16. Diffusion marine (Yuan et al. 2019, EPSL)

```
∂h/∂t = ∇·(D(z) · ∇h),    D(z) = d0 · exp(−profondeur / depth_scale_m)
```

Elle s'applique aux cellules de la mer du pas, avec des côtes à flux nul (rien ne remonte
sur la terre). Les cellules `FIXED` sont des hauteurs imposées qui absorbent ce qui les
atteint (exporté). **Seul `sed` bouge, `b` ne change jamais.**

**Deux classes, résolues séparément** : les grossiers (gravier et sable, `sand_d0`, peu
mobiles, qui restent près de la côte) puis les fines (argile, `fines_d0`, 5 à 20 fois plus
mobiles, qui partent au large). J'ai écarté le `D0` moyen pondéré par `sed_frac` : il
déplacerait tout ensemble, sans aucun tri. Pour chaque classe, trois étapes :

1. **Essai implicite.** On diffuse `h` avec le `D` de la classe, comme si tout le fond en
   était fait. Le schéma est implicite par direction (Thomas en x par ligne, puis en y par
   colonne). Peaceman–Rachford ne convient pas ici : `r = D·dt/dx²` atteint 1e3 à 1e4, et
   il faudrait autant de sous-pas, alors que ce découpage reste monotone pour tout `r`. On
   en tire les flux à travers chaque face de cellule.
2. **Limitation par la disponibilité.** On parcourt les cellules de la plus haute à la plus
   basse de la surface d'essai (le sens des flux), en trois passages au plus pour faire
   suivre ce qui arrive en retard. Chaque face transporte au plus son flux d'essai. Une
   cellule envoie au plus le sédiment de la classe qu'elle porte, y compris ce qu'elle vient
   de recevoir : le sédiment peut ainsi traverser du rocher nu en un seul pas.
3. **Couche mobile** (`mobile_layer_m`, 1 m). Les flux sortants sont multipliés par
   `min(1, sédiment / mobile_layer_m)`. En diffusion de `h` sur une pente rocheuse, une
   pellicule transporterait autant qu'une couverture épaisse, donc à une vitesse illimitée.
   Sans ce facteur, 90 % d'un dépôt d'essai sortait de la carte en un seul pas.

Le bilan est exact, et `b` n'est jamais touché (test `testMarineDiffusion`, qui vérifie
aussi que l'argile part en moyenne plus de 1,5 fois plus loin que le sable).

**Échelle des `d0`.** Les valeurs publiées (de 100 à 1 000 m²/an) valent pour des marges de
centaines de km. Ici, une carte fait quelques km : les longueurs sont environ 100 fois plus
petites, et `D` varie comme le carré des longueurs. Les défauts sont donc :
- `sand_d0` : log [0,05 ; 1] ;
- `fines_d0` : 5 à 20 fois `sand_d0` ;
- `depth_scale_m` : [15 ; 60].

## 17. Bilan global

À chaque pas, sur les cellules non `FIXED` :

```
roche érodée (b avant − b après, hors soulèvement) = Δ sed stocké + exporté (FIXED)
```

L'exporté comprend le fluvial qui atteint une cellule `FIXED` et le marin qui y arrive. Le log
donne une ligne `budget` par intervalle : les volumes, la dérive relative de l'intervalle et
la dérive cumulée, ainsi que le nombre moyen d'itérations de Gauss–Seidel. **La dérive est
de l'ordre de 1e-15** : la comptabilité est exacte par construction (tous les dépôts sont
plafonnés par ce qui arrive, et tous les prélèvements passent par `Regolith`). Les tests
exigent moins de 1e-9.

## 18. Validation de la partie 3

| Config / test | Attendu | Observé |
|---------------|---------|---------|
| `testFluvialRegression` | `G = 0` : exactement la partie 1 | écart < 1e-9 m, 1 itération |
| `testFluvialDeposition` | plus de dépôt quand `G` augmente | dépôt à terre croissant avec `G` |
| `testLakeFilling` | une cuvette de 20 m se remplit | sédiment au centre, profondeur réduite |
| `testMarineDiffusion` | `b` intact, rien sur la terre, fines plus loin | argile à 27 cellules, sable à 8 |
| [validation_deposition.jsonc](../configs/validation_deposition.jsonc) | plaines, cônes, deltas en éventail, tri sable / argile | deltas visibles dans `deposition_rate`, plaine basse au nord-est, anneau d'argile au large |
| toutes les configs | bilan < 1 % | dérive ≈ 1e-15 |

**Ce qui se voit sur le fond marin.** Le sédiment traverse le talus raide sans s'y arrêter :
sur cette pente, les flux sont grands et la couche mobile est atteinte. Il s'accumule au
pied, là où `D(z)` devient faible. Entre le delta près de la côte et cet anneau au large, le
talus reste donc presque nu. Le plateau se remplit d'abord jusqu'au niveau marin, puis
l'export augmente : sur le cas d'équilibre de la partie 1, tout est d'abord stocké, puis
90 % est exporté au bout de 2 Ma.

**Limites connues.**
- **Coût.** Le pas est 2 à 3 fois plus cher qu'à la partie 2. Le solveur fluvial (Gauss–Seidel
  et passe finale) et la diffusion marine (limitation séquentielle) dominent, le parcours
  des deltas reste mineur. Ordre de grandeur à 512² : 50 à 90 ms par pas sans diffusion de
  versant forte, environ 400 ms avec 11 sous-pas ADI.
- **Deltas.** Le remplissage jusqu'au niveau marin, en largeur, donne des deltas en
  demi-disque, sans chenaux distributaires (ce serait de l'avulsion).

---

# Correctifs : durées et équilibre soulèvement / érosion

## 19. Pourquoi les îles devenaient plates

Le relief d'équilibre des rivières sous un soulèvement `U` vaut à peu près `c · U / K`
(loi de puissance à l'état stationnaire). Or `U` et `k_base` étaient tirés indépendamment
(`U` sur 2 ordres de grandeur, `k_base` sur 3), et les durées allaient jusqu'à 20 Ma :

- **`k_base` fort et `U` faible** : un relief d'équilibre de quelques mètres. L'île était
  rabotée au niveau de la mer en quelques centaines de milliers d'années (la seed 3 avait
  0 m dès 2,6 Ma).
- **Phases terminées** : au-delà de la dernière phase, l'érosion seule ramène tout au niveau
  de base, et une durée de 20 Ma suffit largement.

## 20. Calibration (`wgen <config> --calibrate [--resolution N]`)

Île continentale fixe (seed 7, un lobe, R ≈ 1,2 km), un seul dôme à taux constant pendant
toute la simulation, une seule roche (érodabilité 1). Rivières seules : pas de dépôt, pas de
versants, niveau marin constant, pluie de 1 m/an. On simule jusqu'à l'équilibre
(`mean |dh/dt|` < 1 % de `U`, au plus 30 Ma). Seul `world.*` est lu dans la config.

Résultats à 1024² (`dx` = 4 m), `m` = 0,45 :

| U (m/an) | k_base | U / K | max h (m) | h 99 % (m) | h moyen (m) | équilibre |
|---|---|---|---|---|---|---|
| 1e-04 | 1e-06 | 100 | 1322 | 1015 | 116 | 13,6 Ma |
| 1e-04 | 3e-06 | 33 | 480 | 341 | 57 | 5,8 Ma |
| 1e-04 | 1e-05 | 10 | 304 | 102 | 18 | 2,2 Ma |
| 1e-04 | 3e-05 | 3 | 50 | 35 | 7 | 1,6 Ma |
| 5e-04 | 1e-06 | 500 | 6618 | 5110 | 605 | 13,8 Ma |
| 5e-04 | 3e-06 | 167 | 2406 | 1576 | 144 | 6,2 Ma |
| 5e-04 | 1e-05 | 50 | 3188 | 457 | 41 | 2,8 Ma |
| 5e-04 | 3e-05 | 17 | 11742 | 153 | 14 | 2,2 Ma |
| 1e-03 | 1e-06 | 1000 | 12013 | 9546 | 866 | 12,2 Ma |
| 1e-03 | 3e-06 | 333 | 4369 | 3265 | 344 | 4,6 Ma |
| 1e-03 | 1e-05 | 100 | 1465 | 957 | 99 | 2,0 Ma |
| 1e-03 | 3e-05 | 33 | 477 | 305 | 28 | 1,8 Ma |

**Lecture du tableau :**
- **Pics isolés :** `max h` contient des pics d'une cellule (11 742 m pour une moyenne de
  14 m). Sans versants, rien ne limite la pente d'une crête d'une cellule. Le 99ᵉ
  percentile est la bonne mesure des sommets.
- **Constante `c` :** `h 99 %` ≈ **9,1 à 10,5 × U / K** sur toute la grille, d'où `c` = 10 m
  (à `P` = 1 m/an ; le relief varie en `P^-m`).
- **Temps d'équilibre :** de 1,6 à 14 Ma. Il diminue quand `K` augmente et reste presque
  indépendant de `U`.
- **Résolution :** le relief en dépend. À 128², `max h` vaut 880 m au lieu de 1 322 pour
  `U / K` = 100. La constante est prise à 1024², la résolution par défaut.

## 21. Le correctif

**Durées** (`simulation.total_duration_years`) : de 2e5 à 2e6 ans, plus longues pour une
île grande (R de 500 m à 2 km) et vieille.

**Soulèvement en cours** (`tectonics.ongoing_uplift`). La dernière phase de soulèvement
(`collision`, `dome`, `block_uplift`) commence dans le premier cinquième de la simulation et
dure jusqu'à la fin, avec un taux de 60 à 100 % du maximum. C'est le cas de 75 % des îles
continentales, et d'une proportion `1 − âge` des îles volcaniques : un volcan jeune grandit
encore (un dôme : inflation magmatique, nouvelles coulées), et il a alors toujours sa phase.
Les autres sont des îles « vieilles », usées une fois leurs phases terminées.

**Le soulèvement suit l'érodabilité.** La distribution de `k_base` ne change pas. On tire
un relief cible (`tectonics.target_relief_m`), et le taux maximal en est déduit :

```
target_relief_m      cont. log [150, 1500], volc. 50 à 100 % de la hauteur du cône, séd. et cor. [5, 30] ; jeune = plus haut
max_uplift_rate      = target · max(k_base · P^m / 10,  1 / durée), borné à [1e-5, 1e-2] m/an
```

- **Terme `k_base · P^m / 10`** : le relief d'équilibre vaut alors le relief cible.
- **Terme `1 / durée`** : avec un `k_base` faible, l'équilibre demande 5 à 15 Ma, plus que
  toute simulation. Le taux doit alors construire le relief dans le temps imparti, sinon
  l'île ne monte que de quelques mètres.

Les deux paramètres restent forçables. Le relief réel est plus bas que la cible, d'un
facteur 2 à 5, parce que les versants (diffusion, effondrement à l'angle de talus) rabotent
les pentes que les rivières seules rendraient très raides. La calibration ne les inclut pas.

**Ce qui reste : la vitesse d'usure sans soulèvement.** Une île « vieille » ou un volcan éteint
s'use au rythme fixé par `k_base` seul. Le temps d'équilibre du tableau en donne l'échelle :
2 Ma environ pour `k_base` ≥ 1e-5. Exemple, la seed 3 de `island_params.jsonc` : un volcan
éteint, avec un cône de 600 m qui tombe à 23 m en 1,24 Ma (`k_base` = 1,9e-5, basalte
d'érodabilité 0,6). La distribution de `k_base` (log [1e-6, 1e-4]) n'a pas été touchée.

**Vérification** sur 7 seeds continentales à 512² : toutes construisent et gardent leur
relief (de 230 à 480 m en fin de simulation pour 6 d'entre elles, 137 m pour la seed 3), alors
qu'avant, la seed 3 tombait à 0 m. Le test `testDurationsAndUplift` vérifie les durées, la
formule du taux et la proportion de soulèvements en cours.

---

# Partie 4 : climat et érosion marine

## 22. Température ([Climate](../src/wg/sim/Climate.hpp))

Tous les `simulation.climate_update_interval_steps` pas :

```
T = base_temperature_c + offset(t) − lapse_rate_c_per_km · max(s, 0) / 1000,    s = h + glace
```

`offset(t)` est la somme des glaciations actives (`timeline.glaciations`), chacune avec des
rampes sur 10 % de sa durée. La glace vaut 0 jusqu'à la partie 5.

## 23. Précipitations orographiques (advection de l'humidité)

Recalculées au même rythme. `wind.direction_deg` est la direction **d'où vient** le vent,
comme en météorologie (de même pour la houle).

1. **Ordre :** les cellules sont triées de l'amont du vent vers l'aval (`dot(p, vent)`). Le
   tri est fait une fois par direction, au début.
2. **Humidité entrante** : interpolation bilinéaire de `q` au point situé **1,5 cellule** en
   amont. À une cellule, l'une des quatre cellules lues pourrait ne pas encore être calculée
   (vent diagonal) ; à 1,5, elles le sont toutes. Au bord amont, l'air arrive de la mer :
   `q = humidité · q_sat(T_mer)`, avec une humidité d'au moins 0,05.
3. Pour chaque cellule :

   ```
   q_sat  = q0 · exp(0.07 · T)
   montée = max(0, dot(vent, ∇s)) · dx            (en km)
   P      = q · (base_rate · dx + condensation_rate · montée) + max(0, q − q_sat)
   P      = min(P, q) ;  q −= P
   en mer : q relaxe vers humidité · q_sat(T), de moisture_recharge_over_sea par km
   ```

   **Écart avec ta formule :** `base_rate` est par km parcouru et `condensation_rate` par km
   de montée. Exprimés par cellule, ils dépendraient de la résolution : une cellule de 4 m ou
   de 32 m ne fait pas pleuvoir autant.
4. **Mise à l'échelle :** `P` est d'abord légèrement flouté (écart-type de 20 m, contre les
   traînées le long du vent). Il est ensuite mis à l'échelle pour que **la moyenne sur la
   terre** vaille `base_precipitation_m_per_year`. Le relief **redistribue** la pluie sans
   changer le budget de l'île, sur lequel `k_base` est calibré. Avec une plaine côtière comme
   référence, les montagnes, qui vident presque toute l'humidité, multipliaient la moyenne
   par 7.
5. **Variabilité du vent :** avec `wind.variability_deg` > 0, on fait la moyenne de 3 passes,
   le vent tourné de −v, 0 et +v.

`climate.orographic.rain_shadow_strength` est **réservé** : l'ombre pluviométrique vient de
l'épuisement de l'humidité au vent. Une amélioration future serait la théorie linéaire de
Smith & Barstow (2004).

**Débit** : si `erosion.fluvial.precipitation_weighted`, `Q_i` cumule `P_i · dx²` au lieu
d'une pluie uniforme.

## 24. Érosion par les vagues ([Coastal](../src/wg/sim/Coastal.hpp))

**Exposition** d'une cellule de côte (terre, `h ≥ niveau marin`, avec un voisin dans
l'océan). Elle est recalculée tous les `erosion.coastal.exposure_update_interval_steps` pas,
en parallèle :

```
exposition = Σ w(θ) · min(distance, fetch_max) / fetch_max / Σ w(θ),    w(θ) = max(0, cos(θ − houle))²
```

Il y a `fetch_ray_count` rayons sur 360°, qui avancent sur l'eau jusqu'à la terre, au fetch
maximal ou au bord de la carte (au-delà : la haute mer). **Écart avec ta formule :** je
normalise par `Σ w`, d'où une valeur dans [0, 1], qui vaut 1 pour un cap face à la houle.

Trois détails, découverts sur le cas des falaises :
- **Diffraction :** la terre à moins de 2 cellules ne bloque pas un rayon, car la houle
  contourne les petites irrégularités. Sinon, une encoche d'une ou deux cellules s'abritait
  elle-même, construisait une plage devant elle, et toute la falaise s'arrêtait.
- **Nouvelles côtes :** une cellule qui rejoint la côte entre deux mises à jour (éboulis,
  plage, terre découverte par la mer) calcule tout de suite son exposition. Avec une
  exposition nulle, elle ne s'érodait pas et masquait la falaise derrière elle.
- **Ordre :** l'exposition des cellules qui deviennent côte par recul est calculée à la fin
  du pas, une fois toutes les destructions faites. Calculée trop tôt, une cellule voyait
  encore l'ancienne côte de ses voisines.

**Recul par accumulateur** :

```
usure_i  += wave_erosion_coeff · houle.energy · exposition_i / dureté_i · dx · dt   (m³)
volume_i  = (h_i − (niveau marin − platform_depth_m)) · dx²
usure_i ≥ volume_i  →  la cellule est détruite jusqu'à la plateforme ;
                       l'excédent passe aux voisins terrestres qui deviennent côte
```

**Pourquoi un accumulateur.** Abaisser chaque cellule de côte un peu à chaque pas donnerait
une pente douce au lieu d'une falaise. L'accumulateur fait reculer la falaise cellule par
cellule, en gardant toute sa hauteur.

- **`wave_erosion_coeff`** est en m² de section de falaise par an et par mètre de côte (log
  [0,05 ; 5]). Une falaise de 20 m, de dureté 0,5, face à la houle, recule donc de 0,005 à
  0,5 m/an.
- **`platform_depth_m`** est positif ([3, 15] m sous le niveau marin).
- **Dureté de la colonne :** roche et sédiment, pondérés par leur épaisseur au-dessus de la
  plateforme. La dureté de la roche est celle de `material(b)` ; celle du sédiment vaut
  `erosion.coastal.sediment_hardness` (0,1).
- **Excédent :** il ne passe qu'aux cellules qui **deviennent** côte. Les voisines déjà sur
  la côte s'usent à leur propre rythme ; leur passer l'excédent égalisait le recul des roches
  dures et tendres.

**Débris**, avec la composition de ce qui est détruit :
- **côte abritée** (`énergie · exposition ≤ sediment_export_threshold`) : des plages sur les
  cellules marines voisines les plus abritées (le plus de terre autour), jusqu'à 1 m
  au-dessus de la mer ;
- **le reste, et tout sur une côte exposée,** remplit la mer jusqu'à 1 m sous la plateforme,
  de proche en proche (au plus 4 096 cellules). Ce qui ne tient pas, réduit en fines, est
  **exporté**. Une falaise de 30 m produit bien plus de débris que la plateforme ne peut en
  contenir : laissés dessus, ils formaient une terre devant la falaise et la protégeaient.
- **Balayage :** devant une côte exposée (3 cellules), le sédiment au-dessus de la plateforme
  (les éboulis de la falaise par exemple) est emporté au large de la même façon. Les côtes
  abritées gardent leurs plages.

## 25. Niveau marin variable

Tout suit `sea_level(t)`, évalué au milieu du pas :
- les exutoires du Priority-Flood et la mer du pas (l'océan du routage) ;
- les lacs, la terre des versants et la limite absorbante de la diffusion de versant ;
- la diffusion marine et sa profondeur ;
- la côte, l'exposition, la plateforme et les plages.

Seules les cellules `FIXED` gardent leur profondeur absolue. La ligne `budget` du log affiche
le niveau marin courant et le volume de falaise détruit.

## 26. Contrôles

Le bilan global du § 17 inclut l'érosion côtière (la roche détruite est stockée en sédiment
ou exportée) : la dérive reste de l'ordre de 1e-15 à 1e-13.

## 27. Validation de la partie 4

| Config / test | Attendu | Mesuré |
|---|---|---|
| [validation_rain_shadow.jsonc](../configs/validation_rain_shadow.jsonc) | versant au vent humide, versant sous le vent sec ; inversion avec le vent | vent d'ouest : 5,8 m/an à l'ouest, 0,95 à l'est ; vent d'est : 1,2 contre 5,2 |
| (même config) | le versant humide plus raviné | cellules de chenal : 29 % contre 4 % (vent d'ouest), 5 % contre 25 % (vent d'est) |
| `testRainShadow` | idem sur une crête synthétique ; moyenne sur la terre = taux de base | P au vent > 3 × P sous le vent |
| [validation_cliffs.jsonc](../configs/validation_cliffs.jsonc) | falaises verticales de toute la hauteur, plateforme sous l'eau, plages dans les baies abritées | falaise de 0,5 à 24,8 m en une cellule, plateforme rocheuse à −6 m presque nue (5 cm), plages côté est |
| `testCoastalRetreat` | roche tendre plus rapide, falaise de pleine hauteur, plateforme, volumes exacts | recul de 43 cellules (dur) contre 120 à 129 (tendre), comme prévu par le calcul |
| [validation_coast_geology.jsonc](../configs/validation_coast_geology.jsonc) | baies dans le shale, caps dans le gneiss | **contraste faible** sur l'île (voir les limites) |
| [validation_terraces.jsonc](../configs/validation_terraces.jsonc) | terrasses étagées après une baisse par paliers | trois plateformes rocheuses à −6, −16 et −26 m (6 m sous 0, −10 et −20 m), séparées par d'anciennes falaises |

**Limites connues.**
- **Érosion différentielle des côtes :** elle marche sur une côte droite
  (`testCoastalRetreat`). Sur l'île plissée, le contraste mesuré entre gneiss et shale reste
  faible ; il est brouillé par la géométrie de l'île. La dureté est celle de la roche au
  sommet de `b`, comme demandé. La roche au niveau des vagues, celle qui est réellement
  attaquée, serait plus juste.
- **Pentes des falaises :** à 512², l'effondrement thermique limite une falaise à l'angle de
  talus de sa roche par cellule de 8 m. La falaise du cas Ouessant tient pourtant en une
  cellule : le recul est plus rapide que l'effondrement.
- **Rias :** une remontée du niveau marin noie bien les vallées (tout suit `sea_level(t)`),
  mais je ne l'ai pas validé par un cas dédié.

