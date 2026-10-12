# Étape 3 : érosion hydraulique par particules (`erosion`)

Façonner le relief de l'étape 2 avec des gouttes d'eau qui descendent la pente, arrachent de
la matière là où elles accélèrent et la déposent là où elles ralentissent. Le modèle est
**SimpleHydrology** de Nick McDonald, dans sa version de 2023 (« Procedural Hydrology:
Improvements and Meandering Rivers in Particle-Based Hydraulic Erosion Simulations »), avec
les corrections de sa bibliothèque **soillib**. Principe : **peu de règles, simples**.

← [Étape 2](etape-2-initialisation.md) · [Vue d'ensemble](README.md)

Code : [erosion/](../src/wg/erosion/) (grille, goutte, boucle des pas) et
[ErosionStage.cpp](../src/wg/gen/stages/ErosionStage.cpp) (conversion des calques, suivi).

**État :** jalons A (gouttes, transfert de masse, débit), B (avalanche thermique,
matériaux), C (élan) et D (mer). À venir : interaction avec la mer (D), performance (E), réglages
par origine (F).

---

## 1. Repère normalisé

L'algorithme compare des différences de hauteur à la taille d'une cellule. Il travaille donc
dans un repère où **une cellule mesure 1 unité** et où les hauteurs sont exprimées en
`erosion.particles.height_scale_m`, **depuis le niveau de la mer** (la mer est à 0) :

```
h_sim = (h_m − niveau de la mer) / height_scale_m          à l'entrée de l'étape
h_m   = niveau de la mer + h_sim · height_scale_m          à la sortie
```

**Valeur par défaut : la taille d'une cellule** (4 m en 1024² sur 4096 blocs), c'est-à-dire
la géométrie vraie, sans exagération verticale. Pourquoi :

- **Le transfert de masse ne dépend pas de l'échelle.** `c_eq`, le sédiment et les hauteurs
  sont linéaires en `h` : multiplier les hauteurs par k ne change pas l'érosion exprimée en
  mètres.
- **L'échelle n'agit que par la normale** : `|n.xz| = s / √(1 + s²)` (s la pente), qui règle
  la part de la gravité face à l'élan (jalon C). Pour une pente faible, changer
  `height_scale_m` revient à changer `gravity`.
- **soillib travaille déjà ainsi** : hauteurs en « cellules » (80 × un bruit sur 512²),
  talus à 0,8 par cellule (38,7°). Ses pentes de test sont plus raides que les nôtres
  (médiane 36° contre 12 à 16° sur nos îles), mais l'imiter demanderait d'exagérer le relief.
- **Les angles se convertissent tels quels** : `tan(angle) · cellM / height_scale_m`, soit
  `tan(angle)` par défaut, quelle que soit la résolution.

## 2. Données ([ErosionMap.hpp](../src/wg/erosion/ErosionMap.hpp))

Une grille de cellules en **tableau de structures** (une goutte lit et écrit presque tous les
champs de sa cellule à la fois) :

| Champ | Contenu |
|-------|---------|
| `height` | sol : roche + sédiments |
| `sediment` | épaisseur de sédiments meubles (comprise dans `height`) |
| `discharge`, `discharge_track` | débit moyenné dans le temps, et son accumulation pendant le pas |
| `momentum_x/y`, `*_track` | élan moyen de l'eau (jalon C) |

Le flag `FIXED` (anneau de bord) est rangé à côté : ces cellules ne sont jamais modifiées, et
ce qui les atteint quitte le domaine.

## 3. Algorithme

### Un pas ([ParticleErosion.cpp](../src/wg/erosion/ParticleErosion.cpp))

```
pour chaque goutte du pas : naître sur une cellule de terre au hasard, descendre jusqu'à la mort
pour chaque cellule :
    discharge = (1 − discharge_lrate) · discharge + discharge_lrate · discharge_track
    momentum  = (1 − discharge_lrate) · momentum  + discharge_lrate · momentum_track
    discharge_track = momentum_track = 0
```

- **Nombre de gouttes** (`particles_per_step` null ou 0) : une pour 512 cellules de terre,
  comptées au départ. C'est la densité des deux références (512 gouttes par pas sur 512²), qui
  garde `discharge_scale = 0,4` dans son régime de réglage.
- **Naissance** : uniforme sur les cellules de terre (`h > 0`, hors `FIXED`), par tirage avec
  rejet. Pour pondérer plus tard par une carte de pluie, il suffira d'accepter un candidat
  avec une probabilité donnée par cette carte (`ParticleErosion::spawn`).
- **Déterminisme** : les gouttes du pas k viennent d'un flux aléatoire dérivé de
  `(seed de l'étape, k)`.

### Une goutte ([Drop.cpp](../src/wg/erosion/Drop.cpp))

Volume 1, sédiment 0, vitesse nulle. À chaque itération, sur la cellule `floor(pos)` :

1. **Mort** sur place si `age > max_age`, si `volume < min_volume` ou après
   `underwater_steps` itérations en mer : elle dépose tout ce qu'elle porte sur cette cellule.
   **En mer** (`h < 0`), la goutte continue donc quelques itérations : sa gravité est réduite
   à 30 % (elle file surtout sur son élan, un jet dans la mer) et elle ne fait que déposer
   (`c_eq = 0`, jamais d'érosion sous l'eau). Son sédiment se dépose en avançant : deltas et
   plages devant les embouchures.
2. **Gravité** : `g` le gradient de hauteur (différences centrées, décentrées au bord),
   `n = normalize(−g.x, 1, −g.y)`, `speed += gravity · (n.x, n.z) / volume`.
   **Élan** : le courant moyen de la cellule (`momentum`) entraîne la goutte, d'autant plus
   qu'ils vont déjà dans le même sens :
   ```
   si |momentum| > 1e−6 et |speed| > 0 :
       speed += momentum_transfer · dot(n̂(momentum), n̂(speed)) / (volume + discharge) · momentum
   ```
   Le seuil écarte les cellules où le courant s'est éteint : sa moyenne décroît jusqu'aux
   float dénormalisés, où le produit des normes tombait à 0 et la division semait des NaN
   (après environ 1500 pas).
3. **Pas de temps dynamique** : `speed` est ramenée à la norme √2. La goutte avance donc
   d'environ une cellule et n'en saute jamais. Une vitesse nulle la tue (dépôt sur place).
4. **Débit et élan** : `discharge_track += volume` et `momentum_track += volume · speed` sur
   la cellule courante.
5. **Déplacement** : `pos += speed`. Hors de la grille ou sur une cellule `FIXED`, la goutte
   meurt et son sédiment est **perdu** (compté dans le bilan).
6. **Transfert de masse** sur la cellule quittée :
   ```
   hdiff    = h(ancienne) − h(nouvelle)
   c_eq     = max(0, (1 + entrainment · erf(discharge_scale · discharge(ancienne))) · hdiff)
   transfer = max(deposition_rate · (c_eq · volume − sediment), −sediment)
   sediment += transfer ;  h(ancienne) −= transfer
   ```
   `transfer > 0` érode la cellule, `< 0` y dépose. L'érosion retire d'abord les sédiments
   meubles au taux normal, puis la roche à son taux : `erodibility^hardness_influence` de son
   matériau. Un dépôt augmente `height` et `sediment`. La cellule modifiée subit ensuite
   l'avalanche (section 3.3).
7. **Évaporation** : `volume *= 1 − evaporation_rate`.

**Écarts avec la consigne, alignés sur soillib :**
- **En mer** (jalon D), voir l'étape 1 ci-dessus.
- **Le transfert est pondéré par le volume** (`c_eq · volume`) : une goutte qui s'évapore
  porte moins.
- **L'évaporation ne divise pas le sédiment.** SimpleHydrology faisait
  `sediment /= 1 − evaporation_rate`, ce qui crée de la matière à chaque itération ; soillib
  l'a retiré. Sans cela, la masse est conservée.
- **Un dépôt est limité à ce que porte la goutte**, sans quoi son sédiment deviendrait
  négatif.

### Avalanche thermique ([Avalanche.cpp](../src/wg/erosion/Avalanche.cpp))

Après chaque modification d'une cellule (transfert, dépôt à la mort d'une goutte), pour
chacun de ses 8 voisins :

```
diff    = h(i) − h(j)
limite  = talus(cellule la plus haute) · dist(i, j)          // dist = 1 ou √2
si |diff| > limite :
    transfert = thermal.rate · (|diff| − limite) / 2           // de la plus haute à la plus basse
```

Deux talus par cellule haute :
- **ses sédiments meubles** glissent au-delà du talus des sédiments
  (`thermal.sediment_talus_angle_deg`, ou `thermal.underwater_talus_angle_deg` sous la mer),
  **mais pas plus que leur épaisseur** : un voile de sable ne fait pas tomber une falaise ;
- **sa roche** cède au-delà du talus de son matériau, **au-dessus de la mer seulement** (pas
  d'érosion sous l'eau).

Le transfert est le plus grand des deux. La matière déplacée devient du **sédiment** chez la
cellule basse. Une cellule qui reçoit est vérifiée à son tour,
sur six niveaux au plus, pour que les débris glissent jusqu'au repos. Les cellules `FIXED` ne participent pas.

**Talus de la roche** ([Rock.cpp](../src/wg/erosion/Rock.cpp)) : celui du matériau de sa
surface, `geology.materials[material(x, y, h − sediment)]`. Converti dans le repère normalisé : `tan(angle) · cellM / height_scale_m`. Le plus petit talus de tous sert à
écarter d'emblée les paires stables, sans consulter la géologie.

On compare chaque paire, comme SimpleHydrology. soillib compare plutôt chaque voisin à la
moyenne locale (la cellule et ses 8 voisins), ce qui marque moins la grille ; c'est une
variante à garder en tête si des directions de grille apparaissent.

### Bilan de masse

`Σ h + sédiment sorti du domaine` est constant (aucune goutte n'est vivante entre deux pas).
L'étape le calcule en double et le logue tous les dixièmes du run (`wgen` affiche ces
messages) : la dérive reste de l'ordre de 1e−10 de `Σ |h|` après 1000 pas, soit la précision
des float.

## 4. Calques produits

| Calque | Contenu |
|--------|---------|
| `h` | altitude érodée (m) |
| `sediment` | épaisseur de sédiments meubles (m) |
| `discharge_norm` | `erf(discharge_scale · discharge)`, dans [0, 1) : le réseau hydrographique |
| `eroded` | `h_initial − h` (m) : rouge = érosion, bleu = dépôt |
| `momentum` | direction de l'élan moyen de l'eau (radians), style `Direction` : teinte = direction, intensité = norme |
| `momentum_norm` | norme de l'élan moyen |
| `surface_material` | matériau de la roche à nu (sous les sédiments), mis à jour |

## 5. Suivi dans le viewer

L'étape appelle `StageContext::step` après chaque pas. Le pipeline peut alors la mettre en
pause entre deux pas (`PipelineCallbacks::stepGate`) et prendre un aperçu ; `publish` ne
recopie les cellules dans les calques que si un aperçu est pris.

| Touche | Action |
|--------|--------|
| `Espace` | lecture / pause |
| `N` | un pas, pendant la pause |
| `+` / `-` | pas par frame : 1, 2, 4… 64, ou vitesse max (par défaut) |

C'est un pilotage en direct, pas un rejeu : garder l'historique coûterait environ 16 Mo par
image en 1024². Le compteur de pas s'affiche dans la liste des étapes.

## 6. Mesures (jalon A, 1000 pas, 1024², un seul thread, Apple Silicon)

| Île | Gouttes / pas | Temps / pas | Total |
|-----|---------------|-------------|-------|
| volcanique (seed 4, 233 k cellules de terre) | 454 | 5,0 à 5,4 ms | 5,0 à 5,4 s |
| continentale (seed 4, 55 k cellules de terre) | 107 | 1,9 à 2,0 ms | 2 s |

Environ 1,4 ms par pas sont fixes (mesuré avec une seule goutte par pas) : la mise à jour des
cartes parcourt toute la grille (1 M cellules de 32 octets), même quand il y a peu de
gouttes. Le reste croît avec le nombre de gouttes. À traiter au jalon E.

## 7. Jalon B : ce que l'avalanche change (1000 pas, 1024²)

| Île | Pente p99 sans / avec avalanche | Plus grand saut entre voisins | Pic de sédiment |
|-----|-------------------------------|-------------------------------|-----------------|
| volcanique, seed 2 | 60° / 42° | 37 m / 11 m | 17 km (!) / 46 m |
| continentale, seed 4 | 84° / 66° | 107 m / 36 m | 234 m / 89 m |

Temps par pas : environ +20 % (volcanique 6,3 ms, continentale 2,8 ms). Les pentes
au-delà de 60° qui restent sont surtout sur la côte, là où les gouttes déposent en mer (à
traiter au jalon D).

**Limite connue : la dureté des roches n'a presque pas d'effet** dès que l'avalanche est
active. Sur une île continentale à strates alternées granite / schiste argileux plissées,
l'érosion moyenne du granite passe de 36,1 m à 35,8 m quand `hardness_influence` passe de 0 à
1 (sans avalanche : de 19,5 m à 16,8 m). L'avalanche couvre les lits de débris (8 m de
sédiment en moyenne, contre 2,4 m sans elle), et la goutte érode surtout ces sédiments, au
taux normal : la roche à nu, là où la dureté joue, est rarement atteinte. Ni un exposant plus
fort, ni une avalanche freinée par l'érodabilité, ni des montagnes deux fois moins hautes,
ni 4000 pas n'y changent quelque chose. La seule différence entre roches vient de leurs
angles de talus.

## 8. Jalon C : élan

- **Rivières cohérentes** : avec l'élan, les rivières traversent les plateaux d'un seul
  tenant au lieu de s'étaler, et le réseau se hiérarchise nettement.
- **Méandres** : les rivières ondulent et leur tracé migre au fil des pas, mais **sans
  boucles franches ni bras morts** sur nos îles. Elles n'ont pas de vraie plaine alluviale :
  même les plateaux volcaniques gardent quelques degrés de pente. `gravity` règle
  l'équilibre : à 3, les rivières sont plus sinueuses ; à 0,3, elles sont très lisses et
  suivent le courant. `momentum_transfer = 3` change peu.
- **Coût** : nul, voire négatif (les gouttes atteignent la mer plus vite) : 13 à 15 ms par
  pas avec l'élan contre 17 à 19 ms sans, mesurés côte à côte sur une machine chargée.

## 9. Jalon D : la mer (1000 pas, 4 seeds par origine)

**Correction de l'avalanche au passage.** Jusqu'ici, une cellule couverte de sédiments
prenait le talus des sédiments pour toute sa colonne : quelques centimètres de sable sur une
falaise la faisaient s'effondrer à 33° (18° sous la mer), roche comprise. Sur un atoll, le
tombant du récif (48°, sur un socle de basalte à 45°) s'effondrait à chaque dépôt et une
cellule de côte perdait 161 m de roche. Les sédiments glissent maintenant seuls, dans la
limite de leur épaisseur, et la roche sous-marine ne cède plus.

| Critère | Résultat |
|---------|----------|
| éventails devant les embouchures | oui, en lobes aux grandes embouchures des îles volcaniques ; tablier continu autour des îles continentales |
| pics de sédiment sous l'eau | aucun pic isolé ; sédiments trop raides (> 18° et > 0,5 m) : 5 207 cellules sur l'atoll le plus touché (18 479 avant la correction) |
| l'île reste une île | aucune terre dans le dixième extérieur de la carte, sur les 16 îles |
| atolls et îles barrières | changement moyen de 0,2 à 0,4 m sur terre (atolls 1 et 2 : 5 à 12 m), surface de terre entre −7 % et +9 % |

**À noter : les îles continentales gagnent 30 à 55 % de terre.** Leurs montagnes, plus raides
que les talus, s'effondrent et l'érosion (45 m en moyenne) comble le plateau continental
peu profond (−10 à −60 m) : des plaines côtières apparaissent. C'est là que les méandres ont
de la place, mais la silhouette de l'île change nettement.
