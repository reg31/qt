#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR"
SEARCH_DIRS=("qtbase/src" "qtdeclarative/src")
EXCLUDE_PATTERNS=("*/3rdparty/*" "*/doc/*" "*/examples/*" "*/tests/*")
LOG_FILE="$ROOT_DIR/m_object_conversion.log"
DRY_RUN=true

SKIP_MACROS=(
    "Q_PROPERTY"
    "Q_ENUM"
    "Q_FLAG"
    "Q_CLASSINFO"
    "Q_INTERFACES"
    "Q_PLUGIN_METADATA"
    "Q_DECLARE_INTERFACE"
    "Q_REVISION"
    "Q_INVOKABLE"
    "Q_SCRIPTABLE"
    "QML_ELEMENT"
    "QML_NAMED_ELEMENT"
    "QML_ANONYMOUS"
    "QML_INTERFACE"
    "QML_SINGLETON"
    "QML_EXTENDED"
    "QML_FOREIGN"
    "QML_ATTACHED"
    "QDOC_PROPERTY"
)

total_files=0
converted_files=0
skipped_files=0

if [ "$1" = "--apply" ]; then
    DRY_RUN=false
    echo "APPLYING CHANGES"
else
    echo "DRY RUN MODE (use --apply to make changes)"
fi

echo "Log file: $LOG_FILE" | tee "$LOG_FILE"
echo ""

should_exclude() {
    local file="$1"
    for pattern in "${EXCLUDE_PATTERNS[@]}"; do
        if [[ "$file" == $pattern ]]; then
            return 0
        fi
    done
    return 1
}

extract_class_name() {
    local file="$1"
    local qobject_line="$2"
    
    awk -v start="$qobject_line" '
        NR < start { next }
        /^[[:space:]]*class[[:space:]]+/ {
            match($0, /class[[:space:]]+([A-Za-z_][A-Za-z0-9_]*)/, arr)
            if (arr[1] != "") {
                print arr[1]
                exit
            }
        }
    ' "$file" | head -1
}

has_skip_macros() {
    local file="$1"
    local qobject_line="$2"
    
    local access_line=$(awk -v start="$qobject_line" '
        NR <= start { next }
        /^[[:space:]]*(public|private|protected)[[:space:]]*:/ {
            print NR
            exit
        }
    ' "$file")
    
    if [ -z "$access_line" ]; then
        access_line=$(wc -l < "$file")
    fi
    
    for macro in "${SKIP_MACROS[@]}"; do
        if awk -v start="$qobject_line" -v end="$access_line" -v macro="$macro" '
            NR <= start { next }
            NR > end { exit }
            $0 ~ macro { exit 0 }
            END { exit 1 }
        ' "$file"; then
            echo "$macro"
            return 0
        fi
    done
    
    return 1
}

process_file() {
    local file="$1"
    
    if ! grep -q "Q_OBJECT" "$file"; then
        return
    fi
    
    total_files=$((total_files + 1))
    
    local qobject_line=$(grep -n "Q_OBJECT" "$file" | head -1 | cut -d: -f1)
    
    if [ -z "$qobject_line" ]; then
        return
    fi
    
    local skip_reason=$(has_skip_macros "$file" "$qobject_line")
    if [ $? -eq 0 ]; then
        skipped_files=$((skipped_files + 1))
        echo "SKIP: $file ($skip_reason)" >> "$LOG_FILE"
        return
    fi
    
    local class_name=$(extract_class_name "$file" "$qobject_line")
    
    if [ -z "$class_name" ]; then
        skipped_files=$((skipped_files + 1))
        echo "SKIP: $file (no class name)" >> "$LOG_FILE"
        return
    fi
    
    echo "CONVERT: $file -> $class_name" | tee -a "$LOG_FILE"
    converted_files=$((converted_files + 1))
    
    if [ "$DRY_RUN" = false ]; then
        local temp_file="${file}.tmp"
        
        local has_include=$(grep -c '#include.*m_object.h' "$file" || true)
        if [ "$has_include" -eq 0 ]; then
            awk '
                !done && /#include/ {
                    print "#include \"m_object.h\""
                    done=1
                }
                { print }
            ' "$file" > "$temp_file"
            mv "$temp_file" "$file"
        fi
        
        if [[ "$OSTYPE" == "darwin"* ]]; then
            sed -i '' "s/Q_OBJECT/M_OBJECT($class_name)/g" "$file"
            sed -i '' 's/^[[:space:]]*signals[[:space:]]*:/public:/g' "$file"
            sed -i '' 's/^[[:space:]]*Q_SIGNALS[[:space:]]*:/public:/g' "$file"
            sed -i '' '/^[[:space:]]*slots[[:space:]]*:/d' "$file"
            sed -i '' '/^[[:space:]]*Q_SLOTS[[:space:]]*:/d' "$file"
            sed -i '' 's/^[[:space:]]*public[[:space:]]*slots[[:space:]]*:/public:/g' "$file"
            sed -i '' 's/^[[:space:]]*private[[:space:]]*slots[[:space:]]*:/private:/g' "$file"
            sed -i '' 's/^[[:space:]]*protected[[:space:]]*slots[[:space:]]*:/protected:/g' "$file"
        else
            sed -i "s/Q_OBJECT/M_OBJECT($class_name)/g" "$file"
            sed -i 's/^[[:space:]]*signals[[:space:]]*:/public:/g' "$file"
            sed -i 's/^[[:space:]]*Q_SIGNALS[[:space:]]*:/public:/g' "$file"
            sed -i '/^[[:space:]]*slots[[:space:]]*:/d' "$file"
            sed -i '/^[[:space:]]*Q_SLOTS[[:space:]]*:/d' "$file"
            sed -i 's/^[[:space:]]*public[[:space:]]*slots[[:space:]]*:/public:/g' "$file"
            sed -i 's/^[[:space:]]*private[[:space:]]*slots[[:space:]]*:/private:/g' "$file"
            sed -i 's/^[[:space:]]*protected[[:space:]]*slots[[:space:]]*:/protected:/g' "$file"
        fi
    fi
}

for search_dir in "${SEARCH_DIRS[@]}"; do
    search_path="$ROOT_DIR/$search_dir"
    
    if [ ! -d "$search_path" ]; then
        echo "WARNING: Directory not found: $search_path"
        continue
    fi
    
    echo "Scanning $search_dir..."
    
    while IFS= read -r -d '' file; do
        if should_exclude "$file"; then
            continue
        fi
        
        process_file "$file"
    done < <(find "$search_path" -type f \( -name "*.h" -o -name "*_p.h" \) -print0)
done

echo ""
echo "========================================" | tee -a "$LOG_FILE"
echo "SUMMARY" | tee -a "$LOG_FILE"
echo "========================================" | tee -a "$LOG_FILE"
echo "Total files with Q_OBJECT: $total_files" | tee -a "$LOG_FILE"
echo "Files converted: $converted_files" | tee -a "$LOG_FILE"
echo "Files skipped: $skipped_files" | tee -a "$LOG_FILE"

if [ "$total_files" -gt 0 ]; then
    conversion_rate=$(echo "scale=1; ($converted_files * 100) / $total_files" | bc)
    echo "Conversion rate: ${conversion_rate}%" | tee -a "$LOG_FILE"
else
    echo "Conversion rate: 0.0%" | tee -a "$LOG_FILE"
fi

echo "========================================" | tee -a "$LOG_FILE"

if [ "$DRY_RUN" = true ]; then
    echo ""
    echo "This was a DRY RUN. Use --apply to make actual changes."
fi

exit 0
