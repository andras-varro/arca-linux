#!/bin/bash
# ScummVM Arca Backend Management Script
# This script helps manage your local ScummVM changes without committing to upstream

SCUMMVM_DIR="../scummvm"
BACKEND_FILES_DIR="backend-files"
BACKEND_SRC_DIR="backends/platform/arca"

# Resolve absolute paths before any cd operations
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ABS_BACKEND_FILES="$SCRIPT_DIR/$BACKEND_FILES_DIR"
ABS_SCUMMVM_DIR="$(cd "$SCRIPT_DIR/$SCUMMVM_DIR" 2>/dev/null && pwd)"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo "ScummVM Arca Backend Management"
echo "======================================"
echo ""

# Function to check current status
check_status() {
    echo -e "${GREEN}Checking git status...${NC}"
    ( cd "$ABS_SCUMMVM_DIR" || exit 1
    git status
    echo ""
    
    echo -e "${GREEN}Files marked with skip-worktree:${NC}"
    git ls-files -v | grep '^S'
    echo ""
    
    echo -e "${GREEN}Local exclude patterns:${NC}"
    cat .git/info/exclude
    )
    
    echo ""
    echo -e "${GREEN}Backend files status:${NC}"
    if [ -d "$ABS_BACKEND_FILES" ]; then
        echo "Version-controlled backend files in $BACKEND_FILES_DIR:"
        ls -lh "$ABS_BACKEND_FILES"
    else
        echo -e "${RED}Backend files directory not found!${NC}"
    fi
}

# Function to sync backend files to version control
sync_to_vcs() {
    echo -e "${GREEN}Syncing backend files to version control...${NC}"
    
    # Create backend-files directory if it doesn't exist
    mkdir -p "$ABS_BACKEND_FILES"
    
    # Save configure changes (temporarily disable skip-worktree)
    ( cd "$ABS_SCUMMVM_DIR" || exit 1
    git update-index --no-skip-worktree configure
    git diff configure > "$ABS_BACKEND_FILES/configure.patch"
    git update-index --skip-worktree configure
    )
    
    # Copy backend source files
    if [ -d "$ABS_SCUMMVM_DIR/$BACKEND_SRC_DIR" ]; then
        echo "Copying backend source files..."
        cp "$ABS_SCUMMVM_DIR/$BACKEND_SRC_DIR"/*.cpp "$ABS_BACKEND_FILES/" 2>/dev/null
        cp "$ABS_SCUMMVM_DIR/$BACKEND_SRC_DIR"/*.h "$ABS_BACKEND_FILES/" 2>/dev/null
        cp "$ABS_SCUMMVM_DIR/$BACKEND_SRC_DIR"/*.mk "$ABS_BACKEND_FILES/" 2>/dev/null
        cp "$ABS_SCUMMVM_DIR/$BACKEND_SRC_DIR"/README.md "$ABS_BACKEND_FILES/" 2>/dev/null
        echo -e "${GREEN}✓ Backend files synced to $BACKEND_FILES_DIR${NC}"
    else
        echo -e "${RED}Backend directory not found in ScummVM!${NC}"
        return 1
    fi
    # Copy OSS mixer (shared Audio hardware driver for ScummVM)
    OSS_MIXER_DIR="$ABS_SCUMMVM_DIR/backends/mixer/oss"
    if [ -d "$OSS_MIXER_DIR" ]; then
        echo "Copying OSS mixer files..."
        cp "$OSS_MIXER_DIR"/*.cpp "$ABS_BACKEND_FILES/" 2>/dev/null
        cp "$OSS_MIXER_DIR"/*.h   "$ABS_BACKEND_FILES/" 2>/dev/null
        echo -e "${GREEN}✓ OSS mixer files synced to $BACKEND_FILES_DIR${NC}"
    else
        echo -e "${YELLOW}⚠ OSS mixer directory not found — skipping${NC}"
    fi    
    
    echo ""
    echo -e "${GREEN}Files now in version control:${NC}"
    ls -lh "$ABS_BACKEND_FILES"
}

# Function to restore backend files from version control
restore_from_vcs() {
    echo -e "${YELLOW}Restoring backend files from version control...${NC}"
    
    if [ ! -d "$ABS_BACKEND_FILES" ]; then
        echo -e "${RED}Backend files directory not found!${NC}"
        echo "Run 'sync' command first to create version-controlled files."
        return 1
    fi
    
    ( cd "$ABS_SCUMMVM_DIR" || exit 1
    
    # Apply configure patch
    if [ -f "$ABS_BACKEND_FILES/configure.patch" ]; then
        if git apply --check "$ABS_BACKEND_FILES/configure.patch" 2>/dev/null; then
            git apply "$ABS_BACKEND_FILES/configure.patch"
            echo -e "${GREEN}✓ Applied configure.patch${NC}"
        else
            echo -e "${YELLOW}⚠ configure.patch already applied (skipped)${NC}"
        fi
    else
        echo -e "${YELLOW}⚠ configure.patch not found${NC}"
    fi
    
    # Create backend directory if it doesn't exist
    mkdir -p "$BACKEND_SRC_DIR"

    # Restore backend source files
    echo "Restoring backend source files..."
    # OSS mixer goes to its own subdirectory — copy it out first
    OSS_MIXER_DIR="backends/mixer/oss"
    mkdir -p "$OSS_MIXER_DIR"
    if cp "$ABS_BACKEND_FILES/oss-mixer.h"   "$OSS_MIXER_DIR/" 2>/dev/null; then
        echo -e "${GREEN}✓ Restored oss-mixer.h${NC}"
    fi
    if cp "$ABS_BACKEND_FILES/oss-mixer.cpp" "$OSS_MIXER_DIR/" 2>/dev/null; then
        echo -e "${GREEN}✓ Restored oss-mixer.cpp${NC}"
    fi

    # Platform backend files (exclude oss-mixer files which are already placed)
    for f in "$ABS_BACKEND_FILES"/*.cpp "$ABS_BACKEND_FILES"/*.h; do
        fname=$(basename "$f")
        [[ "$fname" == oss-mixer* ]] && continue
        cp "$f" "$BACKEND_SRC_DIR/" 2>/dev/null || true
    done
    cp "$ABS_BACKEND_FILES"/*.mk        "$BACKEND_SRC_DIR/" 2>/dev/null || true
    cp "$ABS_BACKEND_FILES"/README.md   "$BACKEND_SRC_DIR/" 2>/dev/null || true
    echo -e "${GREEN}✓ Restored backend files to $BACKEND_SRC_DIR${NC}"

    # Touch all restored source files so their timestamps are strictly newer
    # than any pre-existing .o files.  Without this, `make` sees the .o as
    # up-to-date and skips recompilation — the root cause of stale-binary bugs.
    touch "$BACKEND_SRC_DIR"/*.cpp "$BACKEND_SRC_DIR"/*.h \
          "$OSS_MIXER_DIR"/*.cpp "$OSS_MIXER_DIR"/*.h 2>/dev/null || true
    echo -e "${GREEN}✓ Touched source files to invalidate stale object cache${NC}"

    # Delete stale .o files so make is forced to recompile even if timestamps
    # are unreliable (e.g. on NTFS/WSL clock-skew).
    rm -f "$BACKEND_SRC_DIR"/*.o "$OSS_MIXER_DIR"/*.o 2>/dev/null || true
    echo -e "${GREEN}✓ Removed stale .o files from build tree${NC}"

    
    )
}

# Function to create backup patch (legacy support)
create_backup() {
    echo -e "${YELLOW}Note: This creates a legacy tar.gz backup.${NC}"
    echo -e "${YELLOW}Consider using 'sync' command for version-controlled files.${NC}"
    echo ""
    echo -e "${GREEN}Creating backup patch...${NC}"
    ( cd "$ABS_SCUMMVM_DIR" || exit 1
    
    # Create patches directory if it doesn't exist
    mkdir -p ../scummvm-patches
    
    # Save configure changes (temporarily disable skip-worktree)
    git update-index --no-skip-worktree configure
    git diff configure > ../scummvm-patches/configure.patch
    git update-index --skip-worktree configure
    
    # Archive custom files
    tar -czf ../scummvm-patches/arca-backend.tar.gz \
        backends/platform/arca/ \
    
    echo -e "${GREEN}✓ Backup created in ../scummvm-patches/${NC}"
    echo "  - configure.patch"
    echo "  - arca-backend.tar.gz"
    )
}

# Function to restore from backup (legacy support)
restore_backup() {
    echo -e "${YELLOW}Note: This restores from legacy tar.gz backup.${NC}"
    echo -e "${YELLOW}Consider using 'restore' command for version-controlled files.${NC}"
    echo ""
    echo -e "${YELLOW}Restoring from backup...${NC}"
    ( cd "$ABS_SCUMMVM_DIR" || exit 1
    
    if [ -f ../scummvm-patches/configure.patch ]; then
        if git apply --check ../scummvm-patches/configure.patch 2>/dev/null; then
            git apply ../scummvm-patches/configure.patch
            echo -e "${GREEN}✓ Applied configure.patch${NC}"
        else
            echo -e "${YELLOW}⚠ configure.patch already applied (skipped)${NC}"
        fi
    fi
    
    if [ -f ../scummvm-patches/arca-backend.tar.gz ]; then
        tar -xzf ../scummvm-patches/arca-backend.tar.gz
        echo -e "${GREEN}✓ Extracted arca-backend.tar.gz${NC}"
    fi
    
    )
}

# Function to update from upstream
update_upstream() {
    echo -e "${YELLOW}Updating from upstream...${NC}"
    
    # First, sync current state to version control
    echo "Syncing current state before update..."
    sync_to_vcs
    
    ( cd "$ABS_SCUMMVM_DIR" || exit 1
    
    # Temporarily disable skip-worktree
    echo "Temporarily disabling skip-worktree..."
    git update-index --no-skip-worktree configure
    
    # Stash local changes
    echo "Stashing local changes..."
    git stash push -m "Arca changes - $(date +%Y%m%d-%H%M%S)"
    
    # Move the tree to the commit pinned in upstream-commit (the build refuses any other).
    echo "Checking out the pinned upstream commit..."
    PIN="$(sed -n 's/^\([0-9a-f]\{40\}\)$/\1/p' "$SCRIPT_DIR/upstream-commit" | head -n 1)"
    [ -n "$PIN" ] || { echo "no 40-hex commit in upstream-commit"; exit 1; }
    git fetch origin branch-2-8
    git checkout --detach "$PIN"
    
    # Restore changes
    echo "Restoring local changes..."
    git stash pop
    
    # Re-enable skip-worktree
    echo "Re-enabling skip-worktree..."
    git update-index --skip-worktree configure
    
    )
    
    # Sync updated state
    echo "Syncing updated state..."
    sync_to_vcs
    
    echo -e "${GREEN}✓ Update complete${NC}"
}

# Function to setup git ignore configuration
setup_ignore() {
    echo -e "${GREEN}Setting up git ignore configuration...${NC}"
    ( cd "$ABS_SCUMMVM_DIR" || exit 1
    
    # Add to local exclude
    cat >> .git/info/exclude << 'EOF'

# Arca backend implementation
backends/platform/arca/

# Arca documentation
EOF
    
    # Mark configure with skip-worktree
    git update-index --skip-worktree configure
    
    echo -e "${GREEN}✓ Git ignore configuration complete${NC}"
    )
}

# Function to reset skip-worktree
reset_skipworktree() {
    echo -e "${YELLOW}Resetting skip-worktree for configure...${NC}"
    ( cd "$ABS_SCUMMVM_DIR" || exit 1
    git update-index --no-skip-worktree configure
    echo -e "${GREEN}✓ Skip-worktree disabled for configure${NC}"
    )
}

# Main menu
case "${1:-}" in
    status)
        check_status
        ;;
    sync)
        sync_to_vcs
        ;;
    restore)
        restore_from_vcs
        ;;
    backup)
        create_backup
        ;;
    backup-restore)
        restore_backup
        ;;
    update)
        update_upstream
        ;;
    setup)
        setup_ignore
        ;;
    reset)
        reset_skipworktree
        ;;
    *)
        echo "Usage: $0 {status|sync|restore|backup|backup-restore|update|setup|reset}"
        echo ""
        echo "Commands:"
        echo "  status         - Check current git status and configuration"
        echo "  sync           - Sync backend files TO version control (recommended)"
        echo "  restore        - Restore backend files FROM version control (recommended)"
        echo "  backup         - Create legacy tar.gz backup (old method)"
        echo "  backup-restore - Restore from legacy tar.gz backup (old method)"
        echo "  update         - Move the tree to the commit pinned in upstream-commit (and sync)"
        echo "  setup          - Setup git ignore configuration (run once)"
        echo "  reset          - Reset skip-worktree for configure file"
        echo ""
        echo "Recommended workflow:"
        echo "  $0 sync     # Sync your changes to version control"
        echo "  $0 update   # Check out the pinned ScummVM commit"
        echo "  $0 restore  # Restore your backend files after update"
        echo ""
        echo "Legacy workflow (tar.gz):"
        echo "  $0 backup          # Create tar.gz backup"
        echo "  $0 backup-restore  # Restore from tar.gz"
        exit 1
        ;;
esac
