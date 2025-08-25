from typing import Optional
from abc import ABC, abstractmethod

class Card:
    symbol: Optional[str]
    command: Optional[str]
    argument: Optional[str]
    comment: Optional[str]
    number: Optional[str]
    
    def __init__(self, text: str):
        full_card = '{:<80}'.format(text[:80])
        
        self.symbol = full_card[0:8].rstrip() or None
        
        if full_card[8] == '*':
            self.comment = full_card[9:72].rstrip() or None
            self.command = None
            self.argument = None
        
        else:
            self.comment = full_card[48:72].rstrip() or None
            self.command = full_card[9:15].rstrip() or None
            self.argument = full_card[15:48].rstrip() or None
        
        self.number = full_card[72:80].rstrip()

def generate_address(
    arg: str,
    symbols: Optional[dict[str, int]] = None
    pc: Optional[int] = 0
) -> int:
    """
    123456          displacement only
    _123456         use direct page
    .123456         PC relative
    123456(4        X4 relative, closing parenthesis optional
    123456+         A13 post-increment
    123456-         A13 pre-decrement
    @               indirect
    $               label (relative to PC if PC relative mode used)
    """
    pass

class AssemblerModule(ABC):
    @abstractmethod
    def size(self, arg: str) -> int:
        pass
    
    def assemble(self, arg: str) -> int:
        pass

class Assembler:
    symbols: dict[str, int]
    
    

"""
        * A longer comment blah blah blah                               00001000
LABEL008 ADD   0,7,'123456701                   This is the comment     00001020

>>> string[0:8].strip()
'LABEL008'
>>> string[8].strip()
''
>>> string[9:15].strip()
'ADD'
>>> string[15:48].strip()
"0,7,'123456701"
>>> string[48:72].strip()
'This is the comment'
>>> string[72:80].strip()
'00001020'
"""