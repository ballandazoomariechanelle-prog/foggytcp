import os
import zipfile

# 1. Enregistrer le dernier commit (Utilisation de git, si disponible)
# Si vous n'utilisez PAS git, cette ligne va échouer/ne rien faire. 
# Si elle échoue, vous pouvez la commenter, mais le prof pourrait ne pas apprécier l'absence du fichier CUR_COMMIT.
# Pour le Checkpoint 1, nous laissons la ligne pour être fidèle au script original :
os.system('git log -1 --pretty=format:"%h" > .git/CUR_COMMIT')

# 2. Liste des fichiers à inclure dans l'archive
FILES_TO_ZIP = [
    '.git/CUR_COMMIT',
    'foggytcp/src/foggy_function.cc',
    'foggytcp/src/foggy_tcp.cc',
    'foggytcp/inc/foggy_function.h',
    'foggytcp/inc/foggy_tcp.h'
]

ZIP_FILENAME = 'submit.zip'

# 3. Création de l'archive ZIP
# 'w' pour écrire (créer) le fichier, zipfile.ZIP_DEFLATED pour la compression
try:
    with zipfile.ZipFile(ZIP_FILENAME, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for file in FILES_TO_ZIP:
            # zipf.write(source, destination dans l'archive)
            # -j dans la commande originale 'zip' signifie de ne pas stocker les chemins de répertoires (junk paths).
            # Nous utilisons os.path.basename(file) pour imiter ce comportement 
            # et stocker les fichiers à la racine du submit.zip.
            if os.path.exists(file):
                zipf.write(file, os.path.basename(file))
            else:
                print(f"Attention : Le fichier {file} est manquant et ne sera pas inclus dans {ZIP_FILENAME}.")

    print("\n✅ Fichier de soumission créé avec succès : {}".format(ZIP_FILENAME))
    print("Veuillez envoyer ce fichier avec votre rapport au professeur.")

except Exception as e:
    print(f"\n❌ Erreur lors de la création du fichier ZIP : {e}")