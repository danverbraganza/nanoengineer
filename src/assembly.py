# Copyright (c) 2004 Nanorex, Inc.  All rights reserved.

"""
assembly.py -- provides class assembly, a set of molecules
(plus selection state) to be shown in one glpane.

Temporarily owned by bruce 041104 for shakedown inval/update code.

$Id$
"""
from Numeric import *
from VQT import *
from string import *
import re
from OpenGL.GL import *
from OpenGL.GLU import *
from OpenGL.GLUT import *
from struct import unpack

from drawer import drawsphere, drawcylinder, drawline, drawaxes
from drawer import segstart, drawsegment, segend, drawwirecube
from shape import *
from chem import *
from gadgets import *
from Utility import *

# number of atoms for detail level 0
HUGE_MODEL = 20000
# number of atoms for detail level 1
LARGE_MODEL = 5000

def hashAtomPos(pos):
    return int(dot(V(1000000, 1000,1),floor(pos*1.2)))

# the class for groups of parts (molecules)
# currently only one level, but should be recursive
class assembly:
    def __init__(self, win, nm=None):
        # nothing is done with this now, but should have a
        # control for browsing/managing the list
        global assyList
        assyList += [self]
        # the MWsemantics displaying this assembly. 
        self.w = win
        # self.mt = win.modelTreeView
        # self.o = win.glpane
        #  ... done in MWsemantics to avoid a circularity
        
        # the name if any
        self.name = nm or gensym("Assembly")
        # all the Nodes in the assembly registered here
        self.dict = {}
        
        # the coordinate system (Actually default view)
        self.csys = Csys(self, "CSys", 10.0, 0.0, 1.0, 0.0, 0.0)
        self.xy = Datum(self, "XY", "plane", V(0,0,0), V(0,0,1))
        self.yz = Datum(self, "YZ", "plane", V(0,0,0), V(1,0,0))
        self.zx = Datum(self, "ZX", "plane", V(0,0,0), V(0,1,0))
        grpl1=[self.csys, self.xy, self.yz, self.zx]
        self.data=Group("Data", self, None, grpl1)
        self.data.open=False

        self.shelf = Group("Clipboard", self, None, [])
        self.shelf.open = False

        # the model tree for this assembly
        self.tree = Group(self.name, self, None)
        self.root = Group("ROOT", self, None, [self.tree, self.shelf])
        # list of chem.molecule's
        self.molecules=[]
        # list of the atoms, only valid just after read or write
        self.alist = [] #None
        # filename if this was read from a file
        self.filename = ""
        # to be shrunk, see addmol
        self.bbox = BBox()
        self.center=V(0,0,0)
        # dictionary of selected atoms (indexed by atom.key)
        self.selatoms={}
        # list of selected molecules
        self.selmols=[]
        # what to select: 0=atoms, 2 = molecules
        self.selwhat = 2
        # level of detail to draw
        self.drawLevel = 2
        # currently unimplemented
        self.selmotors=[]
        self.undolist=[]
        # 1 if there is a structural difference between assy and file
        self.modified = 0
        
        ### Some information needed for the simulation or coming from mmp file
        self.temperature = 300
        self.waals = None

    def selectingWhat(self):
        """return 'Atoms' or 'Molecules' to indicate what is currently
        being selected [by bruce 040927; might change]
        """
        # bruce 040927: this seems to be wrong sometimes,
        # e.g. when no atoms or molecules exist in the assembly... not sure.
        return {0: "Atoms", 2: "Molecules"}[self.selwhat]

    def checkpicked(self, always_print = 0):
        """check whether every atom and molecule has its .picked attribute
        set correctly. Fix errors, too. [bruce 040929]
        """
        if always_print: print "fyi: checkpicked()..."
        self.checkpicked_atoms(always_print = 0)
        self.checkpicked_mols(always_print = 0)
        # maybe do in order depending on selwhat, right one first?? probably doesn't matter.
        #e we ought to call this really often and see when it prints something,
        # so we know what causes the selection bugs i keep hitting.
        # for now see menu1 in select mode (if i committed my debug hacks in there)
        
    def checkpicked_mols(self, always_print = 1):
        "checkpicked, just for molecules [bruce 040929]"
        if always_print: print "fyi: checkpicked_mols()..."
        for mol in self.molecules:
            wantpicked = (mol in self.selmols)
            if mol.picked != wantpicked:
                print "mol %r.picked was %r, should be %r (fixing)" % (mol, mol.picked, wantpicked)
                mol.picked = wantpicked
        return

    def checkpicked_atoms(self, always_print = 1):
        "checkpicked, just for atoms [bruce 040929]"
        if always_print: print "fyi: checkpicked_atoms()..."
        lastmol = None
        for mol in self.molecules:
            for atom in mol.atoms.values(): ##k
                wantpicked = (atom.key in self.selatoms) ##k
                if atom.picked != wantpicked:
                    if mol != lastmol:
                        lastmol = mol
                        print "in mol %r:" % mol
                    print "atom %r.picked was %r, should be %r (fixing)" % (atom, atom.picked, wantpicked)
                    atom.picked = wantpicked
        return
    
    # convert absolute atom positions to relative, find
    # bounding boxes, do some housekeeping

    def addmol(self,mol):
        """(Public method:)
        Add a chunk to this assembly.
        Merge bboxes and update our drawlevel
        (though there is no guarantee that mol's bbox and/or number of atoms
        won't change again during the same user-event that's running now;
        some code might add mol when it has no atoms, then add atoms to it).
        """
        self.modified = 1 #bruce 041118
        self.bbox.merge(mol.bbox)
        self.center = self.bbox.center()
        self.molecules += [mol]
        self.tree.addmember(mol)
   
        self.setDrawLevel()
        
    ## Calculate the number of atoms in an assembly, which is used to
    ## control the detail level of sphere subdivision
    def setDrawLevel(self):

        num = 0
        for mol in self.molecules:
	    num += len(mol.atoms)
        self.drawLevel = 2
        if num > LARGE_MODEL: self.drawLevel = 1
        if num > HUGE_MODEL: self.drawLevel = 0


    # to draw, just draw everything inside
    def draw(self, win):
        self.tree.draw(self.o, self.o.display)
    
    # make a new molecule using a cookie-cut shape
    def molmake(self,shap):
        self.modified = 1 # The file and the part are now out of sync.
        mol = molecule(self, gensym("Cookie."))
        ndx={}
        hashAtomPos
        bbhi, bblo = shap.bbox.data
        # Widen the grid enough to get bonds that cross the box
        griderator = genDiam(bblo-1.6, bbhi+1.6)
        pp=griderator.next()
        while (pp):
            pp0 = pp1 = None
            if shap.isin(pp[0]):
                pp0h = hashAtomPos(pp[0])
                if pp0h not in ndx:
                    pp0 = atom("C", pp[0], mol)
                    ndx[pp0h] = pp0
                else: pp0 = ndx[pp0h]
            if shap.isin(pp[1]):
                pp1h = hashAtomPos(pp[1])
                if pp1h not in ndx:
                    pp1 = atom("C", pp[1], mol)
                    ndx[pp1h] = pp1
                else: pp1 = ndx[pp1h]
            if pp0 and pp1: mol.bond(pp0, pp1)
            elif pp0:
                x = atom("X", (pp[0] + pp[1]) / 2.0, mol)
                mol.bond(pp0, x)
            elif pp1:
                x = atom("X", (pp[0] + pp[1]) / 2.0, mol)
                mol.bond(pp1, x)
            pp=griderator.next()

        #Added by huaicai to fixed some bugs for the 0 atoms molecule 09/30/04
        if len(mol.atoms) > 0:  
            self.addmol(mol)
            self.unpickatoms()
            self.unpickparts()
            self.selwhat = 2
            mol.pick()
            self.mt.update()

    # set up to run a movie or minimization
    def movsetup(self):
        for m in self.molecules:
            m.freeze()
        self.blist = {}
        for a in self.alist:
            for b in a.bonds:
                self.blist[b.key]=b
        pass

    # move the atoms one frame as for movie or minimization
    # .dpb file is in units of 16 pm
    # units here are angstroms
    def movatoms(self, file):
        if not self.alist: return
        ###e bruce 041104 thinks this should first check whether the
        # molecules involved have been updated in an incompatible way
        # (which might change the indices of atoms); otherwise crashes
        # might occur. It might be even worse if a shakedown would run
        # during this replaying! (This is just a guess; I haven't tested
        # it or fully analyzed all related code, or checked whether
        # those dangerous mods are somehow blocked during the replay.)
        for a in self.alist:
            # (assuming mol still frozen, this will change both basepos and
            #  curpos since they are the same object; it won't update or
            #  invalidate other attrs of the mol, however -- ok?? [bruce 041104])
            a.molecule.basepos[a.index] += A(unpack('bbb',file.read(3)))*0.01
        for b in self.blist.itervalues():
            b.setup_invalidate()
            
        for m in self.molecules:
            m.changeapp()

    # regularize the atoms' new positions after the motion
    def movend(self):
        # terrible hack for singlets in simulator, which treats them as H
        for a in self.alist:
            if a.element==Singlet: a.snuggle()
        for m in self.molecules:
            m.unfreeze()
        self.o.paintGL()


    #########################

            # user interface

    #########################

    # functions from the "Select" menu

    def selectAll(self):
        """Select all parts if nothing selected.
        If some parts are selected, select all atoms in those parts.
        If some atoms are selected, select all atoms in the parts
        in which some atoms are selected.
        """
        if self.selwhat:
            for m in self.molecules:
                m.pick()
        else:
            for m in self.molecules:
                for a in m.atoms.itervalues():
                    a.pick()
        self.w.update()


    def selectNone(self):
        self.unpickatoms()
        self.unpickparts()
        self.w.update()


    def selectInvert(self):
        """If some parts are selected, select the other parts instead.
        If some atoms are selected, select all currently unselected
        atoms in parts in which there are currently some selected atoms.
        (And unselect all currently selected atoms.)
        """
        if self.selwhat:
            mollist = []
            for m in self.molecules:
                if m not in self.selmols: mollist.append(m)
            self.unpickparts()
            for m in mollist: m.pick()
        else:
            mollist = []
            for a in self.selatoms.itervalues():
                if a.molecule not in mollist: mollist.append(a.molecule)
            for m in mollist:
                for a in m.atoms.itervalues():
                    if a.picked: a.unpick()
                    else: a.pick()
        self.w.update()

    def selectConnected(self):
        """Select any atom that can be reached from any currently
        selected atom through a sequence of bonds.
        """
        self.marksingle()
        self.w.update()

    def selectDoubly(self):
        """Select any atom that can be reached from any currently
        selected atom through two or more non-overlapping sequences of
        bonds. Also select atoms that are connected to this group by
        one bond and have no other bonds.
        """
        self.markdouble()
        self.w.update()

    def selectAtoms(self):
        self.unpickparts()
        self.selwhat = 0
        self.w.update()
            
    def selectParts(self):
        self.pickParts()
        self.w.update()

    def pickParts(self):
        self.selwhat = 2
        lis = self.selatoms.values()
        self.unpickatoms()
        if lis:
            for a in lis:
                a.molecule.pick()


    # dumb hack: find which atom the cursor is pointing at by
    # checking every atom...
    def findpick(self, p1, v1, r=None, iPic = None, iInv=None):
        distance=1000000
        atom=None
        for mol in self.molecules:
            disp = self.o.display
            if mol.hidden and not iInv: continue
            if mol.display != diDEFAULT: disp = mol.display
            for a in mol.atoms.itervalues():
                if a.display == diINVISIBLE and not iInv: continue
                dist = a.checkpick(p1, v1, disp, r, iPic)
                if dist:
                    if dist<distance:
                        distance=dist
                        atom=a
        return atom

    # make something selected
    def pick(self, p1, v1):
        a = self.findpick(p1, v1)
        if a and self.selwhat: a.molecule.pick()
        elif a: a.pick()

    # make something unselected
    def unpick(self, p1, v1):
        a = self.findpick(p1, v1, None, True, True)
        if a and self.selwhat: a.molecule.unpick()
        elif a: a.unpick()

    # select, make everything else unselected
    def onlypick(self, p1, v1):
        if self.selwhat:
            self.unpickparts()
            self.pickpart(p1, v1)
        else:
            self.unpickatoms()
            self.pick(p1, v1)

    # make an atom selected: deselects all parts
    def pickatom(self, p1, v1):
	self.selwhat = 0
        self.unpickparts()
        if not self.selatoms: self.selatoms = {}
        a = self.findpick(p1, v1)
        if a: a.pick()

    # make a part selected: deselects all atoms
    def pickpart(self, p1, v1):
	self.selwhat = 2
        self.unpickatoms()
        if not self.selmols: self.selmols = []
        a = self.findpick(p1, v1)
        if a: a.molecule.pick()
                
    # deselect any selected atoms
    def unpickatoms(self):
        if self.selatoms:
            for a in self.selatoms.itervalues():
                # this inlines and optims atom.unpick
                a.picked = 0
                a.molecule.changeapp()
            self.selatoms = {}

    # deselect any selected molecules
    def unpickparts(self):
        self.root.unpick()
        self.data.unpick()

    # for debugging
    def prin(self):
        for a in self.selatoms.itervalues():
            a.prin()

    def cut(self):
        if self.selwhat==0: return
        new = Group(gensym("Copy"),self,None)
        self.tree.apply2picked(lambda(x): x.moveto(new))
        if new.members:
            if len(new.members)==1:
                new = new.members[0]
            self.shelf.addmember(new)
            new.pick()
        self.modified = 1
        self.w.update()

    # copy any selected parts (molecules)
    def copy(self):
        if self.selwhat==0: return
        new = Group(gensym("Copy"),self,None)
        # x is each item in the tree that is picked.
        self.tree.apply2picked(lambda(x): new.addmember(x.copy(new)))
        
        if new.members:
            if len(new.members)==1: # If only one member...
                new = new.members[0] # ... make it a single new member.
            self.shelf.addmember(new) # add new member(s) to the clipboard
            new.pick()
        
        # if the new member is a molecule, move the center a little.
        if isinstance(new, molecule): new.move(-new.center)
        self.w.update()

    def paste(self, node):
        pass # to be implemented

    # move any selected parts in space ("move" is an offset vector)
    def movesel(self, move):
        for mol in self.selmols:
            self.modified = 1
            mol.move(move)
 
 
    # rotate any selected parts in space ("rot" is a quaternion)
    def rotsel(self, rot):
        for mol in self.selmols:
            self.modified = 1
            mol.rot(rot)
             

    def kill(self): # bruce 041118 simplified this after shakedown changes
        "delete whatever is selected from this assembly"
        if self.selatoms:
            self.modified = 1
            for a in self.selatoms.values():
                a.kill()
            self.selatoms = {} # should be redundant
        if self.selwhat == 2 or self.selmols:
            self.tree.apply2picked(lambda o: o.kill())
        
        self.shelf.apply2picked(lambda o: o.kill()) # Kill anything picked in the clipboard
            
        self.setDrawLevel()


##    # actually remove a given molecule from the list [no longer used]
##    def killmol(self, mol):
##        mol.kill()
##        self.setDrawLevel()


    def Hide(self):
        "Hide all selected chunks"
        if self.selwhat == 2:
            self.tree.apply2picked(lambda x: x.hide())
            self.w.update()

    def Unhide(self):
        "Hide all selected chunks"
        if self.selwhat == 2:
            self.tree.apply2picked(lambda x: x.unhide())
            self.w.update()

    #bond atoms (cheap hack)
    def Bond(self):
        if not self.selatoms: return
        aa=self.selatoms.values()
        if len(aa)==2:
            self.modified = 1
            aa[0].molecule.bond(aa[0], aa[1])
            #bruce 041028 bugfix: bring following lines inside the 'if'
            aa[0].molecule.changeapp()
            aa[1].molecule.changeapp()
            self.o.paintGL()

    #unbond atoms (cheap hack)
    def Unbond(self):
        if not self.selatoms: return
        self.modified = 1
        aa=self.selatoms.values()
        if len(aa)==2:
            #bruce 041028 bugfix: add [:] to copy following lists,
            # since bust will modify them during the iteration
            for b1 in aa[0].bonds[:]:
                for b2 in aa[1].bonds[:]:
                    if b1 == b2: b1.bust()
        self.o.paintGL()

    #stretch a molecule
    def Stretch(self):
        self.modified = 1
        if not self.selmols: return
        for m in self.selmols:
            m.stretch(1.1)
        self.o.paintGL()

    #weld selected molecules together
    def weld(self):
        if len(self.selmols) < 2: return
        mol = self.selmols[0]
        for m in self.selmols[1:]:
            mol.merge(m)


    def align(self):
        if len(self.selmols) < 2: return
        ax = V(0,0,0)
        for m in self.selmols:
            ax += m.getaxis()
        ax = norm(ax)
        for m in self.selmols:
            m.rot(Q(m.getaxis(),ax))
        self.o.paintGL()
                  

    #############

    def __str__(self):
        return "<Assembly of " + self.filename + ">"

    def computeBoundingBox(self):
        """Compute the bounding box for the assembly. This should be
        called whenever the geomety model has been changed, like new
        parts added, parts/atoms deleted, parts moved/rotated(not view
        move/rotation), etc."""
        
        self.bbox = BBox()
        for mol in self.molecules:
              self.bbox.merge(mol.bbox)
        self.center = self.bbox.center()    
        
    # makes a motor connected to the selected atoms
    # note I don't check for a limit of 25 atoms, but any more
    # will choke the file parser in the simulator
    def makeRotaryMotor(self, sightline):
        if not self.selatoms: return
        self.modified = 1

        m=RotaryMotor(self)
        m.findCenter(self.selatoms.values(), sightline)
        mol = self.selatoms.values()[0].molecule
        mol.dad.addmember(m)
        self.unpickatoms()
      
    # makes a Linear Motor connected to the selected atoms
    # note I don't check for a limit of 25 atoms, but any more
    # will choke the file parser in the simulator
    def makeLinearMotor(self, sightline):
        if not self.selatoms: return
        self.modified = 1
        m = LinearMotor(self)
        m.findCenter(self.selatoms.values(), sightline)
        mol = self.selatoms.values()[0].molecule
        mol.dad.addmember(m)
        self.unpickatoms()

    # makes all the selected atoms grounded
    # same note as above
    def makeground(self):
        if not self.selatoms: return
        self.modified = 1
        m=Ground(self, self.selatoms.values())
        mol = self.selatoms.values()[0].molecule
        mol.dad.addmember(m)
        self.unpickatoms()
        
    # sets the temp of all the selected atoms
    # same note as above
    def makestat(self):
        if not self.selatoms: return
        self.modified = 1
        m=Stat(self, self.selatoms.values())
        mol = self.selatoms.values()[0].molecule
        mol.dad.addmember(m)
        self.unpickatoms()

    # select all atoms connected by a sequence of bonds to
    # an already selected one
    def marksingle(self):
        for a in self.selatoms.values():
            self.conncomp(a, 1)

    # connected components. DFS is elegant!
    # This is called with go=1 from eached already picked atom
    # its only problem is relatively limited stack in Python
    def conncomp(self, atom, go=0):
        if go or not atom.picked:
            atom.pick()
            for a in atom.neighbors(): self.conncomp(a)

    # select all atoms connected by two disjoint sequences of bonds to
    # an already selected one. This picks stiff components but doesn't
    # cross single-bond or single-atom bearings or hinges
    # does select e.g. hydrogens connected to the component and nothing else
    def markdouble(self):
        self.father= {}
        self.stack = []
        self.out = []
        self.dfi={}
        self.p={}
        self.i=0
        for a in self.selatoms.values():
            if a not in self.dfi:
                self.father[a]=None
                self.blocks(a)
        for (a1,a2) in self.out[-1]:
            a1.pick()
            a2.pick()
        for mol in self.molecules:
            for a in mol.atoms.values():
                if len(a.bonds) == 1 and a.neighbors()[0].picked:
                    a.pick()

    # compared to that, the doubly-connected components algo is hairy.
    # cf Gibbons: Algorithmic Graph Theory, Cambridge 1985
    def blocks(self, atom):
        self.dfi[atom]=self.i
        self.p[atom] = self.i
        self.i += 1
        for a2 in atom.neighbors():
            if atom.key < a2.key: pair = (atom, a2)
            else: pair = (a2, atom)
            if not pair in self.stack: self.stack += [pair]
            if a2 in self.dfi:
                if a2 != self.father[atom]:
                    self.p[atom] = min(self.p[atom], self.dfi[a2])
            else:
                self.father[a2] = atom
                self.blocks(a2)
                if self.p[a2] >= self.dfi[atom]:
                    pop = self.stack.index(pair)
                    self.out += [self.stack[pop:]]
                    self.stack = self.stack[:pop]
                self.p[atom] = min(self.p[atom], self.p[a2])

    # separate selected atoms into a new molecule
    # do not break bonds
    #bruce 040929 adding optional arg new_old_callback for use in extrudeMode.py
    def modifySeparate(self, new_old_callback = None):
        """[bruce comment 040929:]
           For each molecule (named N) containing any selected atoms,
           move the selected atoms out of N (but without breaking any bonds)
           into a new molecule which we name N-frag.
           If N is now empty, remove it.
           [new feature 040929:]
           If new_old_callback is provided, then each time we create a new (nonempty) fragment N-frag,
           call it with the 2 args N-frag and N (that is, with the new and old molecules).
           Warning: we pass the old mol N to that callback,
           even if it has no atoms and we deleted it from self.
           (###k is that ok? if not, we'll change this func to use None in place of N.)
        """
        if self.selwhat: return
        
        if not self.selatoms: #always wind up in part pick mode
            #self.pickParts()
            self.w.toolsSelectMolecules()
            # (This would be bad when entering Extrude, but Extrude only calls
            #  us when self.selatoms, so it doesn't happen then.
            #  But it's done below, always, and I don't yet know why that
            #  doesn't cause trouble (or maybe it does). [bruce 041116])
            return
            
        numolist=[]
        for mol in self.molecules:
            numol = molecule(self, mol.name + gensym("-frag"))
            for a in mol.atoms.values():
                if a.picked:
                    a.unpick()
                    a.hopmol(numol)
            if numol.atoms:
                self.addmol(numol)
                numolist+=[numol]
# no longer needed as of 041116:
##                numol.shakedown() #bruce 041104 bugfix? try selatom in depositMode on it...
##                # need to redo the old one too, unless we removed all its atoms
##                if mol.atoms:
##                    mol.shakedown()
##                else:
##                    self.killmol(mol)
                if new_old_callback:
                    new_old_callback(numol, mol) # new feature 040929
                    
        self.unpickatoms()
        #self.pickParts()
        self.w.toolsSelectMolecules()
        for m in numolist: m.pick()
        
        self.w.update()

    def copySelatomFrags(self):
        #bruce 041116, combining modifySeparate and mol.copy; for extrude
        """
           For each molecule (named N) containing any selected atoms,
           copy the selected atoms of N to make a new molecule named N-frag
           (which is not added to the assembly, self). The old mol N is unchanged.
           The copy is done as if by molecule.copy (with cauterize = 1),
           except that all bonds between selected atoms are copied, even if not in same mol.
           Return a list of pairs of new and old molecules [(N1-frag,N1),...].
           (#e Should we optionally return a list of pairs of new and old atoms, too?
           And one of new bonds or singlets and old external atoms, or the equiv?)
        """
        oldmols = {}
        for a in self.selatoms.values():
            m = a.molecule
            oldmols[id(m)] = m ###k could we use key of just m, instead??
        newmols = {}
        for old in oldmols.values():
            numol = molecule(self, mol.name + "-frag")
            newmols[id(old)] = numol # same keys as in oldmols
        nuats = {}
        for a in self.selatoms.values():
            old = a.molecule
            numol = newmols[id(old)]
            a.info = id(a) # lets copied atoms correspond (useful??)
            nuat = a.copy() # uses new copy method as of bruce 041116
            numol.addatom(nuat)
            nuats[id(a)] = nuat
        extern_atoms_bonds = []
        for a in self.selatoms.values():
            assert a.picked
            for b in a.bonds:
                a2 = b.other(a)
                if a2.picked:
                    # copy the bond (even if it's a mol-mol bond), but only once per bond
                    if id(a) < id(a2):
                        bond_atoms(nuats[id(a)], nuats[id(a2)])
                else:
                    # make a singlet instead of this bond (when we're done)
                    extern_atoms_bonds.append( (a,b) ) # ok if several times for one 'a'
                    #e in future, might keep more info
        for a,b in extern_atoms_bonds:
            # compare to code in Bond.unbond(): ###e make common code
            nuat = nuats[id(a)]
            x = atom('X', + b.ubp(a), nuat.molecule)
            bond_atoms(nuat, x)
        res = []
        for old in oldmols.values():
            new = newmols[id(old)]
            res.append( (new,old) )
        return res
        

    # change surface atom types to eliminate dangling bonds
    # a kludgey hack
    def modifyPassivate(self):
        if self.selwhat == 2:
            for m in self.selmols:
                m.passivate(True)
        else:
            for m in self.molecules:
                m.passivate()
                
        self.modified = 1 # could be much smarter
        self.o.paintGL()

    # add hydrogen atoms to each dangling bond
    # ultimately we'll have the button call directly
    def modifyHydrogenate(self):
        self.o.mode.modifyHydrogenate()
        
    # remove hydrogen atoms from every selected atom/molecule
    # ultimately we'll have the button call directly
    def modifyDehydrogenate(self):
        self.o.mode.modifyDehydrogenate()