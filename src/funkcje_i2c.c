#include "mcu_regs.h"
#include "type.h"
#include "funkcje_i2c.h"

static volatile uint32_t I2CMasterState = I2C_IDLE;

static volatile uint8_t *I2CMasterBuffer;
static volatile uint8_t *I2CSlaveBuffer;
static volatile uint32_t I2CReadLength;
static volatile uint32_t I2CWriteLength;

static volatile uint32_t RdIndex = 0;
static volatile uint32_t WrIndex = 0;
static volatile uint8_t I2CAddr;

/*****************************************************************************
** Function name:		I2C_IRQHandler
**
** Descriptions:		I2C interrupt handler, deal with master mode only.
**
** parameters:			None
*****************************************************************************/
void I2C_IRQHandler(void)
{
  uint8_t StatValue;

  /* this handler deals with master read and master write only */
  StatValue = LPC_I2C->STAT;
  switch ( StatValue )
  {
    case 0x08:          /* A Start condition is issued. */
    WrIndex = 0;
    LPC_I2C->DAT = I2CAddr;
    LPC_I2C->CONCLR = (I2CONCLR_SIC | I2CONCLR_STAC);
    I2CMasterState = I2C_STARTED;
    break;

    case 0x10:          /* A repeated started is issued */
    RdIndex = 0;
    /* Send SLA with R bit set, */
    LPC_I2C->DAT = I2CAddr;
    LPC_I2C->CONCLR = (I2CONCLR_SIC | I2CONCLR_STAC);
    I2CMasterState = I2C_RESTARTED;
    break;

    case 0x18:          /* Regardless, it's a ACK */
    if ( I2CMasterState == I2C_STARTED )
    {
      LPC_I2C->DAT = I2CMasterBuffer[WrIndex++];
      I2CMasterState = DATA_ACK;
    }
    LPC_I2C->CONCLR = I2CONCLR_SIC;
    break;

    case 0x28:  /* Data byte has been transmitted, regardless ACK or NACK */
    case 0x30:
    if ( WrIndex < I2CWriteLength )
    {
      LPC_I2C->DAT = I2CMasterBuffer[WrIndex++]; /* this should be the last one */
      I2CMasterState = DATA_ACK;
    }
    else
    {
      if ( I2CReadLength != 0 )
      {
        LPC_I2C->CONSET = I2CONSET_STA; /* Set Repeated-start flag */
        I2CMasterState = I2C_REPEATED_START;
      }
      else
      {
        I2CMasterState = DATA_NACK;
        LPC_I2C->CONSET = I2CONSET_STO;      /* Set Stop flag */
      }
    }
    LPC_I2C->CONCLR = I2CONCLR_SIC;
    break;

    case 0x40:  /* Master Receive, SLA_R has been sent */
    if ( I2CReadLength == 1 )
    {
      /* Will go to State 0x58 */
      LPC_I2C->CONCLR = I2CONCLR_AAC;   /* assert NACK after data is received */
    }
    else
    {
      /* Will go to State 0x50 */
      LPC_I2C->CONSET = I2CONSET_AA;    /* assert ACK after data is received */
    }
    LPC_I2C->CONCLR = I2CONCLR_SIC;
    break;

    case 0x50:  /* Data byte has been received, regardless following ACK or NACK */
    I2CSlaveBuffer[RdIndex++] = LPC_I2C->DAT;
    if ( RdIndex < I2CReadLength )
    {
      I2CMasterState = DATA_ACK;
      LPC_I2C->CONSET = I2CONSET_AA;    /* assert ACK after data is received */
    }
    else
    {
      I2CMasterState = DATA_NACK;
      LPC_I2C->CONCLR = I2CONCLR_AAC;   /* assert NACK on last byte */
    }
    LPC_I2C->CONCLR = I2CONCLR_SIC;
    break;

    case 0x58:
    I2CSlaveBuffer[RdIndex++] = LPC_I2C->DAT;
    I2CMasterState = DATA_NACK;
    LPC_I2C->CONSET = I2CONSET_STO; /* Set Stop flag */
    LPC_I2C->CONCLR = I2CONCLR_SIC; /* Clear SI flag */
    break;

    case 0x20:       /* regardless, it's a NACK */
    case 0x48:
    LPC_I2C->CONCLR = I2CONCLR_SIC;
    I2CMasterState = DATA_NACK;
    break;

    case 0x38:       /* Arbitration lost, in this example, we don't
                    deal with multiple master situation */
    default:
    LPC_I2C->CONCLR = I2CONCLR_SIC;
    break;
  }
  return;
}

/*****************************************************************************
** Function name:		I2CStart
**
** Descriptions:		Create I2C start condition, a timeout
**						value is set if the I2C never gets started,
**						and timed out. It's a fatal error.
**						Returns false if timed out
**
** Parameters:			None
*****************************************************************************/
uint32_t I2CStart( void )
{
  uint32_t timeout = 0;
  uint32_t retVal = FALSE;

  /*--- Issue a start condition ---*/
  LPC_I2C->CONSET = I2CONSET_STA;	/* Set Start flag */

  /*--- Wait until START transmitted ---*/
  while( 1 )
  {
    if ( I2CMasterState == I2C_STARTED )
    {
      retVal = TRUE;
      break;
    }
    if ( timeout >= I2C_MAX_TIMEOUT )
    {
      retVal = FALSE;
      break;
    }
    timeout++;
  }
  return( retVal );
}

/*****************************************************************************
** Function name:		I2CStop
**
** Descriptions:		Set the I2C stop condition, if the routine
**						never exit, it's a fatal bus error.
**						Return true or never return.
**
** parameters:			None
*****************************************************************************/
uint32_t I2CStop( void )
{
  LPC_I2C->CONSET = I2CONSET_STO;  /* Set Stop flag */
  LPC_I2C->CONCLR = I2CONCLR_SIC;  /* Clear SI flag */

  /*--- Wait for STOP detected ---*/
  while( LPC_I2C->CONSET & I2CONSET_STO );
  return TRUE;
}

/*****************************************************************************
** Function name:		I2CInit
**
** Descriptions:		Initialize I2C controller
** 						Return true or false, return false if the I2C
**						interrupt handler was not installed correctly
**
** parameters:			I2c mode is either MASTER or SLAVE
*****************************************************************************/
uint32_t I2CInit( uint32_t I2cMode, uint32_t slaveAddr )
{
  LPC_SYSCON->PRESETCTRL |= (0x1<<1);

  LPC_SYSCON->SYSAHBCLKCTRL |= (1<<5);
  LPC_IOCON->PIO0_4 |= 0x01;		/* I2C SCL */
  LPC_IOCON->PIO0_5 |= 0x01;		/* I2C SDA */

  /*--- Clear flags ---*/
  LPC_I2C->CONCLR = I2CONCLR_AAC | I2CONCLR_SIC | I2CONCLR_STAC | I2CONCLR_I2ENC;

  /*--- Reset registers ---*/

  LPC_I2C->SCLL   = I2SCLL_SCLL;
  LPC_I2C->SCLH   = I2SCLH_SCLH;


  if ( I2cMode == I2CSLAVE )
  {
	LPC_I2C->ADR0 = slaveAddr;
  }

  /* Enable the I2C Interrupt */
  NVIC_EnableIRQ(I2C_IRQn);

  LPC_I2C->CONSET = I2CONSET_I2EN;
  return( TRUE );
}

/*****************************************************************************
** Function name:		I2CEngine
**
** Descriptions:		The routine to complete a I2C transaction
**						from start to stop.
**						Return false only if the
**						start condition can never be generated and
**						timed out.
**
** parameters:			None
*****************************************************************************/
uint32_t I2CEngine( void )
{
  I2CMasterState = I2C_IDLE;
  RdIndex = 0;
  WrIndex = 0;
  if ( I2CStart() != TRUE )
  {
    I2CStop();
    return ( FALSE );
  }

  while ( 1 )
  {
    if ( I2CMasterState == DATA_NACK )
    {
      I2CStop();
      break;
    }
  }
  return ( TRUE );
}

void I2CRead(uint8_t addr, uint8_t *buf, uint32_t len)
{
    I2CAddr = addr | RD_BIT;
    I2CSlaveBuffer = buf;
    I2CReadLength = len;
    I2CWriteLength = 1;

    I2CEngine();
}

void I2CWrite(uint8_t addr, uint8_t* buf, uint32_t len)
{
    I2CAddr = addr;
    I2CMasterBuffer = buf;
    I2CWriteLength = len;
    I2CReadLength = 0;

    I2CEngine();
}
