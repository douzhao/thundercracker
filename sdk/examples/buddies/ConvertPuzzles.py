#!/usr/bin/env python
"""
script to convert JSON puzzles in CubeBuddies format to a .h file that will be built into the game.  
"""

####################################################################################################
####################################################################################################

import sys
import json
import ValidatePuzzles

####################################################################################################
# Utility
####################################################################################################

def MakeSep():
    sep = ''
    for i in range(100):
        sep += '/'
    sep += '\n'
    return sep        

def Id(container, key):
    return key.upper() + '_' + container[key].upper()

def BuddyToId(name):
    return 'BUDDY_' + name.upper()

def BoolToString(value):
    if value:
        return 'true'
    else:
        return 'false'

####################################################################################################
####################################################################################################

def ConvertPuzzles(src, dest):
    with open(src, 'r') as f:
        data = json.load(f)['puzzles']
        
        with open(dest, 'w') as fout:
            # Comment Separator
            sep = MakeSep()
            
            # File Header
            fout.write(sep)
            fout.write('// Generated by %s - Do not edit by hand!\n' % __file__)
            fout.write(sep)
            fout.write('\n')
            
            for i, puzzle in enumerate(data):
                # Puzzle Header
                fout.write(sep)
                fout.write('// Puzzle %d\n' % i)
                fout.write(sep)
                fout.write('\n')
                
                # Cutscene Buddies (Start)
                buddyIds = [BuddyToId(b) for b in puzzle['cutscene_start']['buddies']]
                fout.write('const BuddyId kCutsceneBuddiesStart%d[] = { %s };\n' % (i, ', '.join(buddyIds)))
                
                # Cutscene Lines (Start)
                fout.write('const CutsceneLine kCutsceneLinesStart%d[] =\n' % i)
                fout.write('{\n')
                for line in puzzle['cutscene_start']['lines']:
                    text = line['text'].replace('\n', '\\n')
                    vars = (line['speaker'], Id(line, 'view'), text)
                    fout.write('    CutsceneLine(%d, CutsceneLine::%s, "%s"),\n' % vars)
                fout.write('};\n')
                
                # Cutscene Buddies (End)
                buddyIds = [BuddyToId(b) for b in puzzle['cutscene_end']['buddies']]
                fout.write('const BuddyId kCutsceneBuddiesEnd%d[] = { %s };\n' % (i, ', '.join(buddyIds)))
                
                # Cutscene Lines (End)
                fout.write('const CutsceneLine kCutsceneLinesEnd%d[] =\n' % i)
                fout.write('{\n')
                for line in puzzle['cutscene_end']['lines']:
                    text = line['text'].replace('\n', '\\n')
                    vars = (line['speaker'], Id(line, 'view'), text)
                    fout.write('    CutsceneLine(%d, CutsceneLine::%s, "%s"),\n' % vars)
                fout.write('};\n')
                
                # Buddy IDs
                buddies = puzzle['buddies']
                buddyIds = [BuddyToId(b) for b in buddies]
                fout.write('const BuddyId kBuddies%d[] = { %s };\n' % (i, ', '.join(buddyIds)))
                
                # Pieces (Start)
                fout.write('const Piece kPiecesStart%d[][NUM_SIDES] =\n' % i)
                fout.write('{\n')
                for j, buddy in enumerate(buddies):
                    fout.write('    {\n')
                    for side in ['top', 'left', 'bottom', 'right']:
                        piece = puzzle['buddies'][buddy]['pieces_start'][side]
                        vars = (BuddyToId(piece['buddy']), Id(piece, 'part'))
                        fout.write('        Piece(%s, Piece::%s),\n' % vars)
                    fout.write('    },\n')
                fout.write('};\n')
                
                # Pieces (End)
                fout.write('const Piece kPiecesEnd%d[][NUM_SIDES] =\n' % i)
                fout.write('{\n')
                for j, buddy in enumerate(buddies):
                    fout.write('    {\n')
                    for side in ['top', 'left', 'bottom', 'right']:
                        piece = puzzle['buddies'][buddy]['pieces_end'][side]
                        vars = (BuddyToId(piece['buddy']), Id(piece, 'part'), BoolToString(piece['solve']))
                        fout.write('        Piece(%s, Piece::%s, %s),\n' % vars)
                    fout.write('    },\n')
                fout.write('};\n')
                fout.write('\n')
                
            # Puzzle Array Header
            fout.write(sep)
            fout.write('// Puzzles Array\n')
            fout.write(sep)
            fout.write('\n')
            
            # Puzzles Array
            fout.write('const Puzzle kPuzzles[] =\n')
            fout.write('{\n')
            for i, puzzle in enumerate(data):
                fout.write('    Puzzle(\n')
                fout.write('        %d,\n' % puzzle['book'])
                fout.write('        "%s",\n' % puzzle['title'])
                fout.write('        "%s",\n' % puzzle['clue'])
                fout.write('        kCutsceneBuddiesStart%d, arraysize(kCutsceneBuddiesStart%d),\n' % (i, i))
                fout.write('        kCutsceneLinesStart%d, arraysize(kCutsceneLinesStart%d),\n' % (i, i))
                fout.write('        kCutsceneBuddiesEnd%d, arraysize(kCutsceneBuddiesEnd%d),\n' % (i, i))
                fout.write('        kCutsceneLinesEnd%d, arraysize(kCutsceneLinesEnd%d),\n' % (i, i))
                fout.write('        %d,\n' % puzzle['cutscene_environment'])
                fout.write('        kBuddies%d, arraysize(kBuddies%d),\n' % (i, i))
                fout.write('        %d,\n' % puzzle['shuffles'])
                fout.write('        kPiecesStart%d, kPiecesEnd%d),\n' % (i, i))
            fout.write('};\n')

####################################################################################################
####################################################################################################

if __name__ == "__main__":
    if len(sys.argv[1:]) != 2:
        print "Usage: python %s <json filename> <dest filename>" % __file__
        exit(1)
    else:
        if ValidatePuzzles.ValidatePuzzles(sys.argv[1]):
            ConvertPuzzles(sys.argv[1], sys.argv[2])
            exit(0)
        else:
            exit(1)
    