import os

def copy_files(source_dir, target_dir):
    for f in os.listdir(source_dir):
        source_file = os.path.join(source_dir, f)
        target_file = os.path.join(target_dir, f)
        
        if os.path.isfile(source_file):
            if not os.path.exists(target_dir):
                os.makedirs(target_dir)
            
            if os.path.exists(target_file):
                os.remove(target_file)
            
            open(target_file, "wb").write(open(source_file, "rb").read())
            
        if os.path.isdir(source_file):
            copy_files(source_file, target_file)
