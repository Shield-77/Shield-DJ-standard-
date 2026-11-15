# SHIELD DJ Standard

Lecteur MP3 et OGG pour 3DS, pensé pour Old 3DS et 2DS. Il affiche les pochettes, permet de parcourir la carte SD et propose pitch, profils de basses, réverbération, filtres audio et lecture aléatoire. Le code a été généré avec l'aide de Codex ; Shield-77 conçoit l'application et la teste sur console.

## Télécharger

La dernière release est **[v1.0.36](https://github.com/Shield-77/Shield-DJ-standard-/releases/tag/v1.0.36)**. Installer `ShieldDJStandard.cia` depuis cette release. Le Title ID est `000400000A210300` ; l'application peut coexister avec SHIELD DJ Lite.

La v1.0.36 ajoute une réverbération prolongée facultative, désactivée par défaut, et corrige le temps affiché quand le pitch change. Les versions précédentes ont ajouté sept filtres et le réglage tactile de la réverbération. Voir les [notes techniques](DEVELOPMENT.md) pour l'historique détaillé.

## Utilisation

- Parcourir les dossiers et choisir un MP3 ou un OGG pour lancer la lecture.
- **SELECT** ouvre les options audio. La musique continue pendant la navigation.
- **Y** change le profil de basses ; **X** active ou désactive la lecture aléatoire.
- **L/R** passe à la piste précédente ou suivante. La croix directionnelle règle notamment le pitch pendant la lecture.
- L'écran supérieur affiche la pochette, les informations du morceau et la progression. L'écran inférieur contient la bibliothèque et les commandes.

L'application affiche un guide au premier lancement. Les réglages audio extrêmes peuvent dégrader le rendu sur des fichiers déjà saturés.

## Code source et licences

Le code de la v1.0.36 est dans [`source/`](source/) et [`include/`](include/). Les visuels et polices nécessaires à la compilation sont dans [`assets/`](assets/). Installer devkitPro avec devkitARM, libctru, citro2d/citro3d et les portlibs audio/image, puis lancer `make`. `makerom` et `bannertool` doivent être obtenus séparément ; `MAKEROM` permet de choisir le chemin de makerom. La bannière du **source public** utilise un son silencieux, afin de ne pas redistribuer un effet sonore dont les droits ne sont pas documentés. Le CIA de la release publiée n'est pas modifié.

La licence principale du code est la [GNU GPLv3](LICENSE). Le décodeur minimp3 a sa licence [CC0 séparée](LICENSES/minimp3-CC0.txt) ; les polices ont leurs propres licences dans `assets/fonts/`. Les logos et le nom SHIELD identifient le projet d'origine et ne sont pas accordés par la licence du code. Présenter clairement toute version modifiée comme telle.
