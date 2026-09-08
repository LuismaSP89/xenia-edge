@echo off
setlocal
REM Rebase this fork's edge branch onto has207/xenia-edge edge, push it to the
REM fork and start a CI build (which also publishes a release). See FORK_NOTES.md.

cd /d "%~dp0"

set FORK_REPO=Forgottenshadow89/xenia-edge

echo === Fetching upstream (origin/edge)...
git fetch origin edge || goto :fail

echo === Checking out edge...
git checkout edge || goto :fail

echo === Rebasing edge onto origin/edge...
git rebase origin/edge
if errorlevel 1 (
    echo.
    echo Rebase stopped with conflicts. Resolve them, then run:
    echo     git add ^<files^> ^&^& git rebase --continue
    echo and run this script again.
    goto :fail
)

echo === Fork-only commits on top of upstream:
git log --oneline origin/edge..edge

echo === Pushing edge to fork...
git push --force-with-lease fork edge || goto :fail

echo === Starting CI on the fork...
gh workflow run CI.yml -R %FORK_REPO% --ref edge || goto :fail

echo.
echo Done. Follow the run at https://github.com/%FORK_REPO%/actions
echo The build lands in https://github.com/%FORK_REPO%/releases when it finishes.
exit /b 0

:fail
echo.
echo *** sync-upstream failed.
exit /b 1
